#include <memory>

#include "envoy/admin/v2alpha/config_dump.pb.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"

#include "common/api/os_sys_calls_impl.h"
#include "common/config/metadata.h"
#include "common/network/address_impl.h"
#include "common/network/io_socket_handle_impl.h"
#include "common/network/listen_socket_impl.h"
#include "common/network/socket_option_impl.h"
#include "common/network/utility.h"
#include "common/protobuf/protobuf.h"

#include "server/configuration_impl.h"
#include "server/listener_manager_impl.h"

#include "extensions/filters/listener/original_dst/original_dst.h"
#include "extensions/transport_sockets/tls/ssl_socket.h"

#include "test/mocks/network/mocks.h"
#include "test/mocks/server/mocks.h"
#include "test/server/utility.h"
#include "test/test_common/environment.h"
#include "test/test_common/registry.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/test_base.h"
#include "test/test_common/threadsafe_singleton_injector.h"
#include "test/test_common/utility.h"

#include "absl/strings/escaping.h"
#include "absl/strings/match.h"

using testing::_;
using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;
using testing::Throw;

namespace Envoy {
namespace Server {

class ListenerHandle {
public:
  ListenerHandle() { EXPECT_CALL(*drain_manager_, startParentShutdownSequence()).Times(0); }
  ~ListenerHandle() { onDestroy(); }

  MOCK_METHOD0(onDestroy, void());

  Init::MockTarget target_;
  MockDrainManager* drain_manager_ = new MockDrainManager();
  Configuration::FactoryContext* context_{};
};

class ListenerManagerImplTest : public TestBase {
protected:
  ListenerManagerImplTest() : api_(Api::createApiForTest()) {
    ON_CALL(server_, api()).WillByDefault(ReturnRef(*api_));
    EXPECT_CALL(worker_factory_, createWorker_()).WillOnce(Return(worker_));
    manager_ = std::make_unique<ListenerManagerImpl>(server_, listener_factory_, worker_factory_);
  }

  /**
   * This routing sets up an expectation that does various things:
   * 1) Allows us to track listener destruction via filter factory destruction.
   * 2) Allows us to register for init manager handling much like RDS, etc. would do.
   * 3) Stores the factory context for later use.
   * 4) Creates a mock local drain manager for the listener.
   */
  ListenerHandle* expectListenerCreate(
      bool need_init,
      envoy::api::v2::Listener::DrainType drain_type = envoy::api::v2::Listener_DrainType_DEFAULT) {
    ListenerHandle* raw_listener = new ListenerHandle();
    EXPECT_CALL(listener_factory_, createDrainManager_(drain_type))
        .WillOnce(Return(raw_listener->drain_manager_));
    EXPECT_CALL(listener_factory_, createNetworkFilterFactoryList(_, _))
        .WillOnce(Invoke(
            [raw_listener, need_init](
                const Protobuf::RepeatedPtrField<envoy::api::v2::listener::Filter>&,
                Configuration::FactoryContext& context) -> std::vector<Network::FilterFactoryCb> {
              std::shared_ptr<ListenerHandle> notifier(raw_listener);
              raw_listener->context_ = &context;
              if (need_init) {
                context.initManager().registerTarget(notifier->target_);
              }
              return {[notifier](Network::FilterManager&) -> void {}};
            }));

    return raw_listener;
  }

  void checkStats(uint64_t added, uint64_t modified, uint64_t removed, uint64_t warming,
                  uint64_t active, uint64_t draining) {
    EXPECT_EQ(added, server_.stats_store_.counter("listener_manager.listener_added").value());
    EXPECT_EQ(modified, server_.stats_store_.counter("listener_manager.listener_modified").value());
    EXPECT_EQ(removed, server_.stats_store_.counter("listener_manager.listener_removed").value());
    EXPECT_EQ(warming,
              server_.stats_store_.gauge("listener_manager.total_listeners_warming").value());
    EXPECT_EQ(active,
              server_.stats_store_.gauge("listener_manager.total_listeners_active").value());
    EXPECT_EQ(draining,
              server_.stats_store_.gauge("listener_manager.total_listeners_draining").value());
  }

  void checkConfigDump(const std::string& expected_dump_yaml) {
    auto message_ptr = server_.admin_.config_tracker_.config_tracker_callbacks_["listeners"]();
    const auto& listeners_config_dump =
        dynamic_cast<const envoy::admin::v2alpha::ListenersConfigDump&>(*message_ptr);

    envoy::admin::v2alpha::ListenersConfigDump expected_listeners_config_dump;
    MessageUtil::loadFromYaml(expected_dump_yaml, expected_listeners_config_dump);
    EXPECT_EQ(expected_listeners_config_dump.DebugString(), listeners_config_dump.DebugString());
  }

  NiceMock<MockInstance> server_;
  NiceMock<MockListenerComponentFactory> listener_factory_;
  MockWorker* worker_ = new MockWorker();
  NiceMock<MockWorkerFactory> worker_factory_;
  std::unique_ptr<ListenerManagerImpl> manager_;
  NiceMock<MockGuardDog> guard_dog_;
  Event::SimulatedTimeSystem time_system_;
  Api::ApiPtr api_;
};

class ListenerManagerImplWithRealFiltersTest : public ListenerManagerImplTest {
public:
  ListenerManagerImplWithRealFiltersTest() {
    // Use real filter loading by default.
    ON_CALL(listener_factory_, createNetworkFilterFactoryList(_, _))
        .WillByDefault(Invoke(
            [](const Protobuf::RepeatedPtrField<envoy::api::v2::listener::Filter>& filters,
               Configuration::FactoryContext& context) -> std::vector<Network::FilterFactoryCb> {
              return ProdListenerComponentFactory::createNetworkFilterFactoryList_(filters,
                                                                                   context);
            }));
    ON_CALL(listener_factory_, createListenerFilterFactoryList(_, _))
        .WillByDefault(Invoke(
            [](const Protobuf::RepeatedPtrField<envoy::api::v2::listener::ListenerFilter>& filters,
               Configuration::ListenerFactoryContext& context)
                -> std::vector<Network::ListenerFilterFactoryCb> {
              return ProdListenerComponentFactory::createListenerFilterFactoryList_(filters,
                                                                                    context);
            }));
    socket_ = std::make_unique<NiceMock<Network::MockConnectionSocket>>();
    local_address_.reset(new Network::Address::Ipv4Instance("127.0.0.1", 1234));
    remote_address_.reset(new Network::Address::Ipv4Instance("127.0.0.1", 1234));
  }

  const Network::FilterChain*
  findFilterChain(uint16_t destination_port, bool expect_destination_port_match,
                  const std::string& destination_address, bool expect_destination_address_match,
                  const std::string& server_name, bool expect_server_name_match,
                  const std::string& transport_protocol, bool expect_transport_protocol_match,
                  const std::vector<std::string>& application_protocols,
                  bool expect_application_protocol_match, const std::string& source_address,
                  bool expect_source_type_test, bool expect_source_type_match) {
    int local_addr_calls = expect_destination_port_match ? 2 : 1;
    if (absl::StartsWith(destination_address, "/")) {
      local_address_.reset(new Network::Address::PipeInstance(destination_address));
    } else {
      if (expect_source_type_test) {
        local_addr_calls += 1;
      }
      local_address_.reset(
          new Network::Address::Ipv4Instance(destination_address, destination_port));
    }
    EXPECT_CALL(*socket_, localAddress())
        .Times(local_addr_calls)
        .WillRepeatedly(ReturnRef(local_address_));

    if (expect_destination_address_match) {
      EXPECT_CALL(*socket_, requestedServerName()).WillOnce(Return(absl::string_view(server_name)));
    } else {
      EXPECT_CALL(*socket_, requestedServerName()).Times(0);
    }

    if (expect_server_name_match) {
      EXPECT_CALL(*socket_, detectedTransportProtocol())
          .WillOnce(Return(absl::string_view(transport_protocol)));
    } else {
      EXPECT_CALL(*socket_, detectedTransportProtocol()).Times(0);
    }

    if (expect_transport_protocol_match) {
      EXPECT_CALL(*socket_, requestedApplicationProtocols())
          .WillOnce(ReturnRef(application_protocols));
    } else {
      EXPECT_CALL(*socket_, requestedApplicationProtocols()).Times(0);
    }

    if (expect_application_protocol_match && expect_source_type_test) {
      if (absl::StartsWith(source_address, "/")) {
        remote_address_.reset(new Network::Address::PipeInstance(source_address));
      } else {
        remote_address_.reset(new Network::Address::Ipv4Instance(source_address, 111));
      }
      EXPECT_CALL(*socket_, remoteAddress()).Times(1).WillRepeatedly(ReturnRef(remote_address_));
    } else {
      EXPECT_CALL(*socket_, remoteAddress()).Times(0);
    }

    const Network::FilterChain* result =
        manager_->listeners().back().get().filterChainManager().findFilterChain(*socket_);
    if (expect_destination_port_match && expect_destination_address_match &&
        expect_server_name_match && expect_transport_protocol_match &&
        expect_application_protocol_match && expect_source_type_match) {
      EXPECT_NE(result, nullptr);
    } else {
      EXPECT_EQ(result, nullptr);
    }
    return result;
  }

  /**
   * Create an IPv4 listener with a given name.
   */
  envoy::api::v2::Listener createIPv4Listener(const std::string& name) {
    envoy::api::v2::Listener listener = parseListenerFromV2Yaml(R"EOF(
      address:
        socket_address: { address: 127.0.0.1, port_value: 1111 }
      filter_chains:
      - filters:
    )EOF");
    listener.set_name(name);
    return listener;
  }

  /**
   * Validate that createListenSocket is called once with the expected options.
   */
  void
  expectCreateListenSocket(const envoy::api::v2::core::SocketOption::SocketState& expected_state,
                           Network::Socket::Options::size_type expected_num_options) {
    EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true))
        .WillOnce(Invoke([this, expected_num_options, &expected_state](
                             Network::Address::InstanceConstSharedPtr, Network::Address::SocketType,
                             const Network::Socket::OptionsSharedPtr& options,
                             bool) -> Network::SocketSharedPtr {
          EXPECT_NE(options.get(), nullptr);
          EXPECT_EQ(options->size(), expected_num_options);
          EXPECT_TRUE(
              Network::Socket::applyOptions(options, *listener_factory_.socket_, expected_state));
          return listener_factory_.socket_;
        }));
  }

  /**
   * Validate that setsockopt() is called the expected number of times with the expected options.
   */
  void expectSetsockopt(NiceMock<Api::MockOsSysCalls>& os_sys_calls, int expected_sockopt_level,
                        int expected_sockopt_name, int expected_value,
                        uint32_t expected_num_calls = 1) {
    EXPECT_CALL(os_sys_calls,
                setsockopt_(_, expected_sockopt_level, expected_sockopt_name, _, sizeof(int)))
        .Times(expected_num_calls)
        .WillRepeatedly(
            Invoke([expected_value](int, int, int, const void* optval, socklen_t) -> int {
              EXPECT_EQ(expected_value, *static_cast<const int*>(optval));
              return 0;
            }));
  }

  /**
   * Used by some tests below to validate that, if a given socket option is valid on this platform
   * and set in the Listener, it should result in a call to setsockopt() with the appropriate
   * values.
   */
  void testSocketOption(const envoy::api::v2::Listener& listener,
                        const envoy::api::v2::core::SocketOption::SocketState& expected_state,
                        const Network::SocketOptionName& expected_option, int expected_value,
                        uint32_t expected_num_options = 1) {
    NiceMock<Api::MockOsSysCalls> os_sys_calls;
    TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> os_calls(&os_sys_calls);
    if (expected_option.has_value()) {
      expectCreateListenSocket(expected_state, expected_num_options);
      expectSetsockopt(os_sys_calls, expected_option.value().first, expected_option.value().second,
                       expected_value, expected_num_options);
      manager_->addOrUpdateListener(listener, "", true);
      EXPECT_EQ(1U, manager_->listeners().size());
    } else {
      EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(listener, "", true), EnvoyException,
                                "MockListenerComponentFactory: Setting socket options failed");
      EXPECT_EQ(0U, manager_->listeners().size());
    }
  }

private:
  std::unique_ptr<Network::MockConnectionSocket> socket_;
  Network::Address::InstanceConstSharedPtr local_address_;
  Network::Address::InstanceConstSharedPtr remote_address_;
};

class MockLdsApi : public LdsApi {
public:
  MOCK_CONST_METHOD0(versionInfo, std::string());
};

TEST_F(ListenerManagerImplWithRealFiltersTest, EmptyFilter) {
  const std::string json = R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "filters": []
  }
  )EOF";

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromJson(json), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
  EXPECT_EQ(std::chrono::milliseconds(15000),
            manager_->listeners().front().get().listenerFiltersTimeout());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, DefaultListenerPerConnectionBufferLimit) {
  const std::string json = R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "filters": []
  }
  )EOF";

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromJson(json), "", true);
  EXPECT_EQ(1024 * 1024U, manager_->listeners().back().get().perConnectionBufferLimitBytes());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SetListenerPerConnectionBufferLimit) {
  const std::string json = R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "filters": [],
    "per_connection_buffer_limit_bytes": 8192
  }
  )EOF";

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromJson(json), "", true);
  EXPECT_EQ(8192U, manager_->listeners().back().get().perConnectionBufferLimitBytes());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SslContext) {
  const std::string json = TestEnvironment::substitute(R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "filters" : [],
    "ssl_context" : {
      "cert_chain_file" : "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_uri_cert.pem",
      "private_key_file" : "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_uri_key.pem",
      "ca_cert_file" : "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ca_cert.pem",
      "verify_subject_alt_name" : [
        "localhost",
        "127.0.0.1"
      ]
    }
  }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromJson(json), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {},
                                      true, "8.8.8.8", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, UdpAddress) {
  const std::string proto_text = R"EOF(
    address: {
      socket_address: {
        protocol: UDP
        address: "127.0.0.1"
        port_value: 1234
      }
    }
    filter_chains: {}
  )EOF";
  envoy::api::v2::Listener listener_proto;
  EXPECT_TRUE(Protobuf::TextFormat::ParseFromString(proto_text, &listener_proto));

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_,
              createListenSocket(_, Network::Address::SocketType::Datagram, _, true));
  manager_->addOrUpdateListener(listener_proto, "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, BadListenerConfig) {
  const std::string json = R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "filters": [],
    "test": "a"
  }
  )EOF";

  EXPECT_THROW(manager_->addOrUpdateListener(parseListenerFromJson(json), "", true),
               Json::Exception);
}

TEST_F(ListenerManagerImplWithRealFiltersTest, BadFilterConfig) {
  const std::string json = R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "filters": [
      {
        "foo" : "type",
        "name" : "name",
        "config" : {}
      }
    ]
  }
  )EOF";

  EXPECT_THROW(manager_->addOrUpdateListener(parseListenerFromJson(json), "", true),
               Json::Exception);
}

TEST_F(ListenerManagerImplWithRealFiltersTest, BadFilterName) {
  const std::string json = R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "filters": [
      {
        "name" : "invalid",
        "config" : {}
      }
    ]
  }
  )EOF";

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromJson(json), "", true),
                            EnvoyException,
                            "Didn't find a registered implementation for name: 'invalid'");
}

class TestStatsConfigFactory : public Configuration::NamedNetworkFilterConfigFactory {
public:
  // Configuration::NamedNetworkFilterConfigFactory
  Network::FilterFactoryCb createFilterFactory(const Json::Object&,
                                               Configuration::FactoryContext& context) override {
    context.scope().counter("bar").inc();
    return [](Network::FilterManager&) -> void {};
  }
  std::string name() override { return "stats_test"; }
};

TEST_F(ListenerManagerImplWithRealFiltersTest, StatsScopeTest) {
  Registry::RegisterFactory<TestStatsConfigFactory, Configuration::NamedNetworkFilterConfigFactory>
      registered;

  const std::string json = R"EOF(
  {
    "address": "tcp://127.0.0.1:1234",
    "bind_to_port": false,
    "filters": [
      {
        "name" : "stats_test",
        "config" : {}
      }
    ]
  }
  )EOF";

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, false));
  manager_->addOrUpdateListener(parseListenerFromJson(json), "", true);
  manager_->listeners().front().get().listenerScope().counter("foo").inc();

  EXPECT_EQ(1UL, server_.stats_store_.counter("bar").value());
  EXPECT_EQ(1UL, server_.stats_store_.counter("listener.127.0.0.1_1234.foo").value());
}

TEST_F(ListenerManagerImplTest, NotDefaultListenerFiltersTimeout) {
  const std::string json = R"EOF(
    name: "foo"
    address:
      socket_address: { address: 127.0.0.1, port_value: 10000 }
    filter_chains:
    - filters:
    listener_filters_timeout: 0s
  )EOF";

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(json), "", true));
  EXPECT_EQ(std::chrono::milliseconds(),
            manager_->listeners().front().get().listenerFiltersTimeout());
}

TEST_F(ListenerManagerImplTest, ReversedWriteFilterOrder) {
  const std::string json = R"EOF(
    name: "foo"
    address:
      socket_address: { address: 127.0.0.1, port_value: 10000 }
    filter_chains:
    - filters:
  )EOF";

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(json), "", true));
  EXPECT_TRUE(manager_->listeners().front().get().reverseWriteFilterOrder());
}

TEST_F(ListenerManagerImplTest, ModifyOnlyDrainType) {
  InSequence s;

  // Add foo listener.
  const std::string listener_foo_yaml = R"EOF(
    name: "foo"
    address:
      socket_address: { address: 127.0.0.1, port_value: 10000 }
    filter_chains:
    - filters:
    drain_type: MODIFY_ONLY
  )EOF";

  ListenerHandle* listener_foo =
      expectListenerCreate(false, envoy::api::v2::Listener_DrainType_MODIFY_ONLY);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_foo_yaml), "", true));
  checkStats(1, 0, 0, 0, 1, 0);

  EXPECT_CALL(*listener_foo, onDestroy());
}

TEST_F(ListenerManagerImplTest, AddListenerAddressNotMatching) {
  InSequence s;

  // Add foo listener.
  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": [],
    "drain_type": "default"
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(false);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  checkStats(1, 0, 0, 0, 1, 0);

  // Update foo listener, but with a different address. Should throw.
  const std::string listener_foo_different_address_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1235",
    "filters": [],
    "drain_type": "modify_only"
  }
  )EOF";

  ListenerHandle* listener_foo_different_address =
      expectListenerCreate(false, envoy::api::v2::Listener_DrainType_MODIFY_ONLY);
  EXPECT_CALL(*listener_foo_different_address, onDestroy());
  EXPECT_THROW_WITH_MESSAGE(
      manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_different_address_json), "",
                                    true),
      EnvoyException,
      "error updating listener: 'foo' has a different address "
      "'127.0.0.1:1235' from existing listener");

  EXPECT_CALL(*listener_foo, onDestroy());
}

// Make sure that a listener creation does not fail on IPv4 only setups when FilterChainMatch is not
// specified and we try to create default CidrRange. See convertDestinationIPsMapToTrie function for
// more details.
TEST_F(ListenerManagerImplTest, AddListenerOnIpv4OnlySetups) {
  InSequence s;

  NiceMock<Api::MockOsSysCalls> os_sys_calls;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> os_calls(&os_sys_calls);

  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": [],
    "drain_type": "default"
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(false);

  EXPECT_CALL(os_sys_calls, socket(AF_INET, _, 0)).WillOnce(Return(Api::SysCallIntResult{5, 0}));
  EXPECT_CALL(os_sys_calls, socket(AF_INET6, _, 0)).WillOnce(Return(Api::SysCallIntResult{-1, 0}));

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));

  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  checkStats(1, 0, 0, 0, 1, 0);
  EXPECT_CALL(*listener_foo, onDestroy());
}

// Make sure that a listener creation does not fail on IPv6 only setups when FilterChainMatch is not
// specified and we try to create default CidrRange. See convertDestinationIPsMapToTrie function for
// more details.
TEST_F(ListenerManagerImplTest, AddListenerOnIpv6OnlySetups) {
  InSequence s;

  NiceMock<Api::MockOsSysCalls> os_sys_calls;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> os_calls(&os_sys_calls);

  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://[::0001]:1234",
    "filters": [],
    "drain_type": "default"
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(false);

  EXPECT_CALL(os_sys_calls, socket(AF_INET, _, 0)).WillOnce(Return(Api::SysCallIntResult{-1, 0}));
  EXPECT_CALL(os_sys_calls, socket(AF_INET6, _, 0)).WillOnce(Return(Api::SysCallIntResult{5, 0}));

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));

  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  checkStats(1, 0, 0, 0, 1, 0);
  EXPECT_CALL(*listener_foo, onDestroy());
}

// Make sure that a listener that is not modifiable cannot be updated or removed.
TEST_F(ListenerManagerImplTest, UpdateRemoveNotModifiableListener) {
  time_system_.setSystemTime(std::chrono::milliseconds(1001001001001));

  InSequence s;

  // Add foo listener.
  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": []
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(false);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", false));
  checkStats(1, 0, 0, 0, 1, 0);
  checkConfigDump(R"EOF(
static_listeners:
  listener:
    name: "foo"
    address:
      socket_address:
        address: "127.0.0.1"
        port_value: 1234
    filter_chains: {}
  last_updated:
    seconds: 1001001001
    nanos: 1000000
dynamic_active_listeners:
dynamic_warming_listeners:
dynamic_draining_listeners:
)EOF");

  // Update foo listener. Should be blocked.
  const std::string listener_foo_update1_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": [
      { "name" : "fake", "config" : {} }
    ]
  }
  )EOF";

  EXPECT_FALSE(
      manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_update1_json), "", false));
  checkStats(1, 0, 0, 0, 1, 0);

  // Remove foo listener. Should be blocked.
  EXPECT_FALSE(manager_->removeListener("foo"));
  checkStats(1, 0, 0, 0, 1, 0);

  EXPECT_CALL(*listener_foo, onDestroy());
}

TEST_F(ListenerManagerImplTest, AddOrUpdateListener) {
  time_system_.setSystemTime(std::chrono::milliseconds(1001001001001));

  InSequence s;

  MockLdsApi* lds_api = new MockLdsApi();
  EXPECT_CALL(listener_factory_, createLdsApi_(_)).WillOnce(Return(lds_api));
  envoy::api::v2::core::ConfigSource lds_config;
  manager_->createLdsApi(lds_config);

  EXPECT_CALL(*lds_api, versionInfo()).WillOnce(Return(""));
  checkConfigDump(R"EOF(
static_listeners:
dynamic_active_listeners:
dynamic_warming_listeners:
dynamic_draining_listeners:
)EOF");

  // Add foo listener.
  const std::string listener_foo_yaml = R"EOF(
name: "foo"
address:
  socket_address:
    address: "127.0.0.1"
    port_value: 1234
filter_chains: {}
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(false);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  EXPECT_TRUE(
      manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_foo_yaml), "version1", true));
  checkStats(1, 0, 0, 0, 1, 0);
  EXPECT_CALL(*lds_api, versionInfo()).WillOnce(Return("version1"));
  checkConfigDump(R"EOF(
version_info: version1
static_listeners:
dynamic_active_listeners:
  version_info: "version1"
  listener:
    name: "foo"
    address:
      socket_address:
        address: "127.0.0.1"
        port_value: 1234
    filter_chains: {}
  last_updated:
    seconds: 1001001001
    nanos: 1000000
dynamic_warming_listeners:
dynamic_draining_listeners:
)EOF");

  // Update duplicate should be a NOP.
  EXPECT_FALSE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_foo_yaml), "", true));
  checkStats(1, 0, 0, 0, 1, 0);

  // Update foo listener. Should share socket.
  const std::string listener_foo_update1_yaml = R"EOF(
name: "foo"
address:
  socket_address:
    address: "127.0.0.1"
    port_value: 1234
filter_chains: {}
per_connection_buffer_limit_bytes: 10
  )EOF";

  time_system_.setSystemTime(std::chrono::milliseconds(2002002002002));

  ListenerHandle* listener_foo_update1 = expectListenerCreate(false);
  EXPECT_CALL(*listener_foo, onDestroy());
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_foo_update1_yaml),
                                            "version2", true));
  checkStats(1, 1, 0, 0, 1, 0);
  EXPECT_CALL(*lds_api, versionInfo()).WillOnce(Return("version2"));
  checkConfigDump(R"EOF(
version_info: version2
static_listeners:
dynamic_active_listeners:
  version_info: "version2"
  listener:
    name: "foo"
    address:
      socket_address:
        address: "127.0.0.1"
        port_value: 1234
    filter_chains: {}
    per_connection_buffer_limit_bytes: 10
  last_updated:
    seconds: 2002002002
    nanos: 2000000
dynamic_warming_listeners:
dynamic_draining_listeners:
)EOF");

  // Start workers.
  EXPECT_CALL(*worker_, addListener(_, _));
  EXPECT_CALL(*worker_, start(_));
  manager_->startWorkers(guard_dog_);
  worker_->callAddCompletion(true);

  // Update duplicate should be a NOP.
  EXPECT_FALSE(
      manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_foo_update1_yaml), "", true));
  checkStats(1, 1, 0, 0, 1, 0);

  time_system_.setSystemTime(std::chrono::milliseconds(3003003003003));

  // Update foo. Should go into warming, have an immediate warming callback, and start immediate
  // removal.
  ListenerHandle* listener_foo_update2 = expectListenerCreate(false);
  EXPECT_CALL(*worker_, addListener(_, _));
  EXPECT_CALL(*worker_, stopListener(_));
  EXPECT_CALL(*listener_foo_update1->drain_manager_, startDrainSequence(_));
  EXPECT_TRUE(
      manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_foo_yaml), "version3", true));
  worker_->callAddCompletion(true);
  checkStats(1, 2, 0, 0, 1, 1);
  EXPECT_CALL(*lds_api, versionInfo()).WillOnce(Return("version3"));
  checkConfigDump(R"EOF(
version_info: version3
static_listeners:
dynamic_active_listeners:
  version_info: "version3"
  listener:
    name: "foo"
    address:
      socket_address:
        address: "127.0.0.1"
        port_value: 1234
    filter_chains: {}
  last_updated:
    seconds: 3003003003
    nanos: 3000000
dynamic_warming_listeners:
dynamic_draining_listeners:
  version_info: "version2"
  listener:
    name: "foo"
    address:
      socket_address:
        address: "127.0.0.1"
        port_value: 1234
    filter_chains: {}
    per_connection_buffer_limit_bytes: 10
  last_updated:
    seconds: 2002002002
    nanos: 2000000
)EOF");

  EXPECT_CALL(*worker_, removeListener(_, _));
  listener_foo_update1->drain_manager_->drain_sequence_completion_();
  checkStats(1, 2, 0, 0, 1, 1);
  EXPECT_CALL(*listener_foo_update1, onDestroy());
  worker_->callRemovalCompletion();
  checkStats(1, 2, 0, 0, 1, 0);

  time_system_.setSystemTime(std::chrono::milliseconds(4004004004004));

  // Add bar listener.
  const std::string listener_bar_yaml = R"EOF(
name: "bar"
address:
  socket_address:
    address: "127.0.0.1"
    port_value: 1235
filter_chains: {}
  )EOF";

  ListenerHandle* listener_bar = expectListenerCreate(false);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  EXPECT_CALL(*worker_, addListener(_, _));
  EXPECT_TRUE(
      manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_bar_yaml), "version4", true));
  EXPECT_EQ(2UL, manager_->listeners().size());
  worker_->callAddCompletion(true);
  checkStats(2, 2, 0, 0, 2, 0);

  time_system_.setSystemTime(std::chrono::milliseconds(5005005005005));

  // Add baz listener, this time requiring initializing.
  const std::string listener_baz_yaml = R"EOF(
name: "baz"
address:
  socket_address:
    address: "127.0.0.1"
    port_value: 1236
filter_chains: {}
  )EOF";

  ListenerHandle* listener_baz = expectListenerCreate(true);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  EXPECT_CALL(listener_baz->target_, initialize(_));
  EXPECT_TRUE(
      manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_baz_yaml), "version5", true));
  EXPECT_EQ(2UL, manager_->listeners().size());
  checkStats(3, 2, 0, 1, 2, 0);
  EXPECT_CALL(*lds_api, versionInfo()).WillOnce(Return("version5"));
  checkConfigDump(R"EOF(
version_info: version5
static_listeners:
dynamic_active_listeners:
  - version_info: "version3"
    listener:
      name: "foo"
      address:
        socket_address:
          address: "127.0.0.1"
          port_value: 1234
      filter_chains: {}
    last_updated:
      seconds: 3003003003
      nanos: 3000000
  - version_info: "version4"
    listener:
      name: "bar"
      address:
        socket_address:
          address: "127.0.0.1"
          port_value: 1235
      filter_chains: {}
    last_updated:
      seconds: 4004004004
      nanos: 4000000
dynamic_warming_listeners:
  - version_info: "version5"
    listener:
      name: "baz"
      address:
        socket_address:
          address: "127.0.0.1"
          port_value: 1236
      filter_chains: {}
    last_updated:
      seconds: 5005005005
      nanos: 5000000
dynamic_draining_listeners:
)EOF");

  // Update a duplicate baz that is currently warming.
  EXPECT_FALSE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(listener_baz_yaml), "", true));
  checkStats(3, 2, 0, 1, 2, 0);

  // Update baz while it is warming.
  const std::string listener_baz_update1_json = R"EOF(
  {
    "name": "baz",
    "address": "tcp://127.0.0.1:1236",
    "filters": [
      { "name" : "fake", "config" : {} }
    ]
  }
  )EOF";

  ListenerHandle* listener_baz_update1 = expectListenerCreate(true);
  EXPECT_CALL(*listener_baz, onDestroy()).WillOnce(Invoke([listener_baz]() -> void {
    // Call the initialize callback during destruction like RDS will.
    listener_baz->target_.callback_();
  }));
  EXPECT_CALL(listener_baz_update1->target_, initialize(_));
  EXPECT_TRUE(
      manager_->addOrUpdateListener(parseListenerFromJson(listener_baz_update1_json), "", true));
  EXPECT_EQ(2UL, manager_->listeners().size());
  checkStats(3, 3, 0, 1, 2, 0);

  // Finish initialization for baz which should make it active.
  EXPECT_CALL(*worker_, addListener(_, _));
  listener_baz_update1->target_.callback_();
  EXPECT_EQ(3UL, manager_->listeners().size());
  worker_->callAddCompletion(true);
  checkStats(3, 3, 0, 0, 3, 0);

  EXPECT_CALL(*listener_foo_update2, onDestroy());
  EXPECT_CALL(*listener_bar, onDestroy());
  EXPECT_CALL(*listener_baz_update1, onDestroy());
}

TEST_F(ListenerManagerImplTest, AddDrainingListener) {
  InSequence s;

  EXPECT_CALL(*worker_, start(_));
  manager_->startWorkers(guard_dog_);

  // Add foo listener directly into active.
  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": []
  }
  )EOF";

  Network::Address::InstanceConstSharedPtr local_address(
      new Network::Address::Ipv4Instance("127.0.0.1", 1234));
  ON_CALL(*listener_factory_.socket_, localAddress()).WillByDefault(ReturnRef(local_address));

  ListenerHandle* listener_foo = expectListenerCreate(false);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  EXPECT_CALL(*worker_, addListener(_, _));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  worker_->callAddCompletion(true);
  checkStats(1, 0, 0, 0, 1, 0);

  // Remove foo into draining.
  EXPECT_CALL(*worker_, stopListener(_));
  EXPECT_CALL(*listener_foo->drain_manager_, startDrainSequence(_));
  EXPECT_TRUE(manager_->removeListener("foo"));
  checkStats(1, 0, 1, 0, 0, 1);
  EXPECT_CALL(*worker_, removeListener(_, _));
  listener_foo->drain_manager_->drain_sequence_completion_();
  checkStats(1, 0, 1, 0, 0, 1);

  // Add foo again. We should use the socket from draining.
  ListenerHandle* listener_foo2 = expectListenerCreate(false);
  EXPECT_CALL(*worker_, addListener(_, _));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  worker_->callAddCompletion(true);
  checkStats(2, 0, 1, 0, 1, 1);

  EXPECT_CALL(*listener_foo, onDestroy());
  worker_->callRemovalCompletion();
  checkStats(2, 0, 1, 0, 1, 0);

  EXPECT_CALL(*listener_foo2, onDestroy());
}

TEST_F(ListenerManagerImplTest, CantBindSocket) {
  InSequence s;

  EXPECT_CALL(*worker_, start(_));
  manager_->startWorkers(guard_dog_);

  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": []
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(true);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true))
      .WillOnce(Throw(EnvoyException("can't bind")));
  EXPECT_CALL(*listener_foo, onDestroy());
  EXPECT_THROW(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true),
               EnvoyException);
}

TEST_F(ListenerManagerImplTest, ListenerDraining) {
  InSequence s;

  EXPECT_CALL(*worker_, start(_));
  manager_->startWorkers(guard_dog_);

  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": []
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(false);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  EXPECT_CALL(*worker_, addListener(_, _));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  worker_->callAddCompletion(true);
  checkStats(1, 0, 0, 0, 1, 0);

  EXPECT_CALL(*listener_foo->drain_manager_, drainClose()).WillOnce(Return(false));
  EXPECT_CALL(server_.drain_manager_, drainClose()).WillOnce(Return(false));
  EXPECT_FALSE(listener_foo->context_->drainDecision().drainClose());

  EXPECT_CALL(*worker_, stopListener(_));
  EXPECT_CALL(*listener_foo->drain_manager_, startDrainSequence(_));
  EXPECT_TRUE(manager_->removeListener("foo"));
  checkStats(1, 0, 1, 0, 0, 1);

  // NOTE: || short circuit here prevents the server drain manager from getting called.
  EXPECT_CALL(*listener_foo->drain_manager_, drainClose()).WillOnce(Return(true));
  EXPECT_TRUE(listener_foo->context_->drainDecision().drainClose());

  EXPECT_CALL(*worker_, removeListener(_, _));
  listener_foo->drain_manager_->drain_sequence_completion_();
  checkStats(1, 0, 1, 0, 0, 1);

  EXPECT_CALL(*listener_foo->drain_manager_, drainClose()).WillOnce(Return(false));
  EXPECT_CALL(server_.drain_manager_, drainClose()).WillOnce(Return(true));
  EXPECT_TRUE(listener_foo->context_->drainDecision().drainClose());

  EXPECT_CALL(*listener_foo, onDestroy());
  worker_->callRemovalCompletion();
  EXPECT_EQ(0UL, manager_->listeners().size());
  checkStats(1, 0, 1, 0, 0, 0);
}

TEST_F(ListenerManagerImplTest, RemoveListener) {
  InSequence s;

  EXPECT_CALL(*worker_, start(_));
  manager_->startWorkers(guard_dog_);

  // Remove an unknown listener.
  EXPECT_FALSE(manager_->removeListener("unknown"));

  // Add foo listener into warming.
  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": []
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(true);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  EXPECT_CALL(listener_foo->target_, initialize(_));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  EXPECT_EQ(0UL, manager_->listeners().size());
  checkStats(1, 0, 0, 1, 0, 0);

  // Remove foo.
  EXPECT_CALL(*listener_foo, onDestroy());
  EXPECT_TRUE(manager_->removeListener("foo"));
  EXPECT_EQ(0UL, manager_->listeners().size());
  checkStats(1, 0, 1, 0, 0, 0);

  // Add foo again and initialize it.
  listener_foo = expectListenerCreate(true);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  EXPECT_CALL(listener_foo->target_, initialize(_));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));
  checkStats(2, 0, 1, 1, 0, 0);
  EXPECT_CALL(*worker_, addListener(_, _));
  listener_foo->target_.callback_();
  worker_->callAddCompletion(true);
  EXPECT_EQ(1UL, manager_->listeners().size());
  checkStats(2, 0, 1, 0, 1, 0);

  // Update foo into warming.
  const std::string listener_foo_update1_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://127.0.0.1:1234",
    "filters": [
      { "name" : "fake", "config" : {} }
    ]
  }
  )EOF";

  ListenerHandle* listener_foo_update1 = expectListenerCreate(true);
  EXPECT_CALL(listener_foo_update1->target_, initialize(_));
  EXPECT_TRUE(
      manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_update1_json), "", true));
  EXPECT_EQ(1UL, manager_->listeners().size());
  checkStats(2, 1, 1, 1, 1, 0);

  // Remove foo which should remove both warming and active.
  EXPECT_CALL(*listener_foo_update1, onDestroy());
  EXPECT_CALL(*worker_, stopListener(_));
  EXPECT_CALL(*listener_foo->drain_manager_, startDrainSequence(_));
  EXPECT_TRUE(manager_->removeListener("foo"));
  checkStats(2, 1, 2, 0, 0, 1);
  EXPECT_CALL(*worker_, removeListener(_, _));
  listener_foo->drain_manager_->drain_sequence_completion_();
  checkStats(2, 1, 2, 0, 0, 1);
  EXPECT_CALL(*listener_foo, onDestroy());
  worker_->callRemovalCompletion();
  EXPECT_EQ(0UL, manager_->listeners().size());
  checkStats(2, 1, 2, 0, 0, 0);
}

TEST_F(ListenerManagerImplTest, AddListenerFailure) {
  InSequence s;

  EXPECT_CALL(*worker_, start(_));
  manager_->startWorkers(guard_dog_);

  // Add foo listener into active.
  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://0.0.0.0:1234",
    "filters": []
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(false);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  EXPECT_CALL(*worker_, addListener(_, _));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));

  EXPECT_CALL(*worker_, stopListener(_));
  EXPECT_CALL(*listener_foo->drain_manager_, startDrainSequence(_));
  worker_->callAddCompletion(false);

  EXPECT_CALL(*worker_, removeListener(_, _));
  listener_foo->drain_manager_->drain_sequence_completion_();

  EXPECT_CALL(*listener_foo, onDestroy());
  worker_->callRemovalCompletion();

  EXPECT_EQ(1UL, server_.stats_store_.counter("listener_manager.listener_create_failure").value());
}

TEST_F(ListenerManagerImplTest, StatsNameValidCharacterTest) {
  const std::string json = R"EOF(
  {
    "address": "tcp://[::1]:10000",
    "filters": [],
    "bind_to_port": false
  }
  )EOF";

  manager_->addOrUpdateListener(parseListenerFromJson(json), "", true);
  manager_->listeners().front().get().listenerScope().counter("foo").inc();

  EXPECT_EQ(1UL, server_.stats_store_.counter("listener.[__1]_10000.foo").value());
}

TEST_F(ListenerManagerImplTest, DuplicateAddressDontBind) {
  InSequence s;

  EXPECT_CALL(*worker_, start(_));
  manager_->startWorkers(guard_dog_);

  // Add foo listener into warming.
  const std::string listener_foo_json = R"EOF(
  {
    "name": "foo",
    "address": "tcp://0.0.0.0:1234",
    "filters": [],
    "bind_to_port": false
  }
  )EOF";

  ListenerHandle* listener_foo = expectListenerCreate(true);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, false));
  EXPECT_CALL(listener_foo->target_, initialize(_));
  EXPECT_TRUE(manager_->addOrUpdateListener(parseListenerFromJson(listener_foo_json), "", true));

  // Add bar with same non-binding address. Should fail.
  const std::string listener_bar_json = R"EOF(
  {
    "name": "bar",
    "address": "tcp://0.0.0.0:1234",
    "filters": [],
    "bind_to_port": false
  }
  )EOF";

  ListenerHandle* listener_bar = expectListenerCreate(true);
  EXPECT_CALL(*listener_bar, onDestroy());
  EXPECT_THROW_WITH_MESSAGE(
      manager_->addOrUpdateListener(parseListenerFromJson(listener_bar_json), "", true),
      EnvoyException,
      "error adding listener: 'bar' has duplicate address '0.0.0.0:1234' as existing listener");

  // Move foo to active and then try to add again. This should still fail.
  EXPECT_CALL(*worker_, addListener(_, _));
  listener_foo->target_.callback_();
  worker_->callAddCompletion(true);

  listener_bar = expectListenerCreate(true);
  EXPECT_CALL(*listener_bar, onDestroy());
  EXPECT_THROW_WITH_MESSAGE(
      manager_->addOrUpdateListener(parseListenerFromJson(listener_bar_json), "", true),
      EnvoyException,
      "error adding listener: 'bar' has duplicate address '0.0.0.0:1234' as existing listener");

  EXPECT_CALL(*listener_foo, onDestroy());
}

TEST_F(ListenerManagerImplTest, EarlyShutdown) {
  // If stopWorkers is called before the workers are started, it should be a no-op: they should be
  // neither started nor stopped.
  EXPECT_CALL(*worker_, start(_)).Times(0);
  EXPECT_CALL(*worker_, stop()).Times(0);
  manager_->stopWorkers();
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithDestinationPortMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        destination_port: 8080
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // IPv4 client connects to unknown port - no match.
  auto filter_chain = findFilterChain(1234, false, "127.0.0.1", false, "", false, "tls", false, {},
                                      false, "8.8.8.8", false, false);
  EXPECT_EQ(filter_chain, nullptr);

  // IPv4 client connects to valid port - using 1st filter chain.
  filter_chain = findFilterChain(8080, true, "127.0.0.1", true, "", true, "tls", true, {}, true,
                                 "8.8.8.8", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  auto ssl_socket =
      dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");

  // UDS client - no match.
  filter_chain = findFilterChain(0, false, "/tmp/test.sock", false, "", false, "tls", false, {},
                                 false, "/tmp/test.sock", false, false);
  EXPECT_EQ(filter_chain, nullptr);
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithDestinationIPMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        prefix_ranges: { address_prefix: 127.0.0.0, prefix_len: 8 }
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // IPv4 client connects to unknown IP - no match.
  auto filter_chain = findFilterChain(1234, true, "1.2.3.4", false, "", false, "tls", false, {},
                                      false, "8.8.8.8", false, false);
  EXPECT_EQ(filter_chain, nullptr);

  // IPv4 client connects to valid IP - using 1st filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {}, true,
                                 "8.8.8.8", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  auto ssl_socket =
      dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");

  // UDS client - no match.
  filter_chain = findFilterChain(0, true, "/tmp/test.sock", false, "", false, "tls", false, {},
                                 false, "/tmp/test.sock", false, false);
  EXPECT_EQ(filter_chain, nullptr);
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithServerNamesMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        server_names: "server1.example.com"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS client without SNI - no match.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", false, "tls", false, {},
                                      false, "8.8.8.8", false, false);
  EXPECT_EQ(filter_chain, nullptr);

  // TLS client without matching SNI - no match.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "www.example.com", false, "tls",
                                 false, {}, false, "8.8.8.8", false, false);
  EXPECT_EQ(filter_chain, nullptr);

  // TLS client with matching SNI - using 1st filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "server1.example.com", true, "tls",
                                 true, {}, true, "8.8.8.8", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  auto ssl_socket =
      dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithTransportProtocolMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        transport_protocol: "tls"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TCP client - no match.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "raw_buffer", false,
                                      {}, false, "8.8.8.8", false, false);
  EXPECT_EQ(filter_chain, nullptr);

  // TLS client - using 1st filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {}, true,
                                 "8.8.8.8", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  auto ssl_socket =
      dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithApplicationProtocolMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        application_protocols: "http/1.1"
        source_type: ANY
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS client without ALPN - no match.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {},
                                      false, "8.8.8.8", false, false);
  EXPECT_EQ(filter_chain, nullptr);

  // TLS client with "http/1.1" ALPN - using 1st filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true,
                                 {"h2", "http/1.1"}, true, "8.8.8.8", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  auto ssl_socket =
      dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");
}

// Define a source_type filter chain match and test against it.
TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithSourceTypeMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        source_type: LOCAL
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // EXTERNAL IPv4 client without "http/1.1" ALPN - no match.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {},
                                      true, "8.8.8.8", true, false);
  EXPECT_EQ(filter_chain, nullptr);

  // LOCAL IPv4 client with "http/1.1" ALPN - using 1st filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true,
                                 {"h2", "http/1.1"}, true, "127.0.0.1", true, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  auto ssl_socket =
      dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");

  // LOCAL UDS client with "http/1.1" ALPN - using 1st filter chain.
  filter_chain = findFilterChain(0, true, "/tmp/test.sock", true, "", true, "tls", true,
                                 {"h2", "http/1.1"}, true, "/tmp/test.sock", true, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  ssl_socket = dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");
}

// Define multiple source_type filter chain matches and test against them.
TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainWithSourceTypeMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        source_type: LOCAL
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
    - filter_chain_match:
        application_protocols: "http/1.1"
        source_type: EXTERNAL
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_uri_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_uri_key.pem" }
    - filter_chain_match:
        source_type: ANY
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_multiple_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_multiple_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // LOCAL TLS client with "http/1.1" ALPN - no match.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true,
                                      {"h2", "http/1.1"}, true, "127.0.0.1", true, false);
  EXPECT_EQ(filter_chain, nullptr);

  // LOCAL TLS client without "http/1.1" ALPN - using 1st filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {}, true,
                                 "127.0.0.1", true, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  auto ssl_socket =
      dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");

  // EXTERNAL TLS client with "http/1.1" ALPN - using 2nd filter chain.
  filter_chain = findFilterChain(1234, true, "8.8.8.8", true, "", true, "tls", true,
                                 {"h2", "http/1.1"}, true, "4.4.4.4", true, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  ssl_socket = dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto uri = ssl_socket->uriSanLocalCertificate();
  EXPECT_EQ(uri, "spiffe://lyft.com/test-team");

  // EXTERNAL TLS client without "http/1.1" ALPN - using 3nd filter chain.
  filter_chain = findFilterChain(1234, true, "8.8.8.8", true, "", true, "tls", true, {}, true,
                                 "4.4.4.4", true, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  ssl_socket = dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 2);
  EXPECT_EQ(server_names.front(), "*.example.com");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithDestinationPortMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        # empty
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_uri_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_uri_key.pem" }
    - filter_chain_match:
        destination_port: 8080
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
    - filter_chain_match:
        destination_port: 8081
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_multiple_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_multiple_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // IPv4 client connects to default port - using 1st filter chain.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {},
                                      true, "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  auto ssl_socket =
      dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto uri = ssl_socket->uriSanLocalCertificate();
  EXPECT_EQ(uri, "spiffe://lyft.com/test-team");

  // IPv4 client connects to port 8080 - using 2nd filter chain.
  filter_chain = findFilterChain(8080, true, "127.0.0.1", true, "", true, "tls", true, {}, true,
                                 "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  ssl_socket = dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");

  // IPv4 client connects to port 8081 - using 3nd filter chain.
  filter_chain = findFilterChain(8081, true, "127.0.0.1", true, "", true, "tls", true, {}, true,
                                 "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  ssl_socket = dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 2);
  EXPECT_EQ(server_names.front(), "*.example.com");

  // UDS client - using 1st filter chain.
  filter_chain = findFilterChain(0, true, "/tmp/test.sock", true, "", true, "tls", true, {}, true,
                                 "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  ssl_socket = dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  uri = ssl_socket->uriSanLocalCertificate();
  EXPECT_EQ(uri, "spiffe://lyft.com/test-team");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithDestinationIPMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        # empty
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_uri_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_uri_key.pem" }
    - filter_chain_match:
        prefix_ranges: { address_prefix: 192.168.0.1, prefix_len: 32 }
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
    - filter_chain_match:
        prefix_ranges: { address_prefix: 192.168.0.0, prefix_len: 16 }
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_multiple_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_multiple_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // IPv4 client connects to default IP - using 1st filter chain.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {},
                                      true, "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  auto ssl_socket =
      dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto uri = ssl_socket->uriSanLocalCertificate();
  EXPECT_EQ(uri, "spiffe://lyft.com/test-team");

  // IPv4 client connects to exact IP match - using 2nd filter chain.
  filter_chain = findFilterChain(1234, true, "192.168.0.1", true, "", true, "tls", true, {}, true,
                                 "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  ssl_socket = dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");

  // IPv4 client connects to wildcard IP match - using 3nd filter chain.
  filter_chain = findFilterChain(1234, true, "192.168.1.1", true, "", true, "tls", true, {}, true,
                                 "192.168.1.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  ssl_socket = dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 2);
  EXPECT_EQ(server_names.front(), "*.example.com");

  // UDS client - using 1st filter chain.
  filter_chain = findFilterChain(0, true, "/tmp/test.sock", true, "", true, "tls", true, {}, true,
                                 "/tmp/test.sock", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  ssl_socket = dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  uri = ssl_socket->uriSanLocalCertificate();
  EXPECT_EQ(uri, "spiffe://lyft.com/test-team");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithServerNamesMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        # empty
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_uri_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_uri_key.pem" }
        session_ticket_keys:
          keys:
          - filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ticket_key_a"
    - filter_chain_match:
        server_names: "server1.example.com"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
        session_ticket_keys:
          keys:
          - filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ticket_key_a"
    - filter_chain_match:
        server_names: "*.com"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_multiple_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_multiple_dns_key.pem" }
        session_ticket_keys:
          keys:
          - filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ticket_key_a"
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS client without SNI - using 1st filter chain.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {},
                                      true, "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  auto ssl_socket =
      dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto uri = ssl_socket->uriSanLocalCertificate();
  EXPECT_EQ(uri, "spiffe://lyft.com/test-team");

  // TLS client with exact SNI match - using 2nd filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "server1.example.com", true, "tls",
                                 true, {}, true, "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  ssl_socket = dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");

  // TLS client with wildcard SNI match - using 3nd filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "server2.example.com", true, "tls",
                                 true, {}, true, "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  ssl_socket = dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 2);
  EXPECT_EQ(server_names.front(), "*.example.com");

  // TLS client with wildcard SNI match - using 3nd filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "www.wildcard.com", true, "tls",
                                 true, {}, true, "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  ssl_socket = dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 2);
  EXPECT_EQ(server_names.front(), "*.example.com");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithTransportProtocolMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        # empty
    - filter_chain_match:
        transport_protocol: "tls"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TCP client - using 1st filter chain.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "raw_buffer", true,
                                      {}, true, "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_FALSE(filter_chain->transportSocketFactory().implementsSecureTransport());

  // TLS client - using 2nd filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {}, true,
                                 "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  auto ssl_socket =
      dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithApplicationProtocolMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        # empty
    - filter_chain_match:
        application_protocols: ["dummy", "h2"]
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS client without ALPN - using 1st filter chain.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {},
                                      true, "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_FALSE(filter_chain->transportSocketFactory().implementsSecureTransport());

  // TLS client with "h2,http/1.1" ALPN - using 2nd filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true,
                                 {"h2", "http/1.1"}, true, "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  auto ssl_socket =
      dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithMultipleRequirementsMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        # empty
    - filter_chain_match:
        server_names: ["www.example.com", "server1.example.com"]
        transport_protocol: "tls"
        application_protocols: ["dummy", "h2"]
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS client without SNI and ALPN - using 1st filter chain.
  auto filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true, {},
                                      true, "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_FALSE(filter_chain->transportSocketFactory().implementsSecureTransport());

  // TLS client with exact SNI match but without ALPN - no match (SNI blackholed by configuration).
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "server1.example.com", true, "tls",
                                 true, {}, false, "127.0.0.1", false, false);
  EXPECT_EQ(filter_chain, nullptr);

  // TLS client with ALPN match but without SNI - using 1st filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "", true, "tls", true,
                                 {"h2", "http/1.1"}, true, "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_FALSE(filter_chain->transportSocketFactory().implementsSecureTransport());

  // TLS client with exact SNI match and ALPN match - using 2nd filter chain.
  filter_chain = findFilterChain(1234, true, "127.0.0.1", true, "server1.example.com", true, "tls",
                                 true, {"h2", "http/1.1"}, true, "127.0.0.1", false, true);
  ASSERT_NE(filter_chain, nullptr);
  EXPECT_TRUE(filter_chain->transportSocketFactory().implementsSecureTransport());
  auto transport_socket = filter_chain->transportSocketFactory().createTransportSocket(nullptr);
  auto ssl_socket =
      dynamic_cast<Extensions::TransportSockets::Tls::SslSocket*>(transport_socket.get());
  auto server_names = ssl_socket->dnsSansLocalCertificate();
  EXPECT_EQ(server_names.size(), 1);
  EXPECT_EQ(server_names.front(), "server1.example.com");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithDifferentSessionTicketKeys) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        server_names: "example.com"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
        session_ticket_keys:
          keys:
          - filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ticket_key_a"
    - filter_chain_match:
        server_names: "www.example.com"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
        session_ticket_keys:
          keys:
          - filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ticket_key_b"
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest,
       MultipleFilterChainsWithMixedUseOfSessionTicketKeys) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        server_names: "example.com"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
        session_ticket_keys:
          keys:
          - filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ticket_key_a"
    - filter_chain_match:
        server_names: "www.example.com"
      tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithInvalidDestinationIPMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        prefix_ranges: { address_prefix: a.b.c.d, prefix_len: 32 }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException, "malformed IP address: a.b.c.d");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SingleFilterChainWithInvalidServerNamesMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        server_names: "*w.example.com"
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException,
                            "error adding listener '127.0.0.1:1234': partial wildcards are not "
                            "supported in \"server_names\"");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, MultipleFilterChainsWithSameMatch) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    listener_filters:
    - name: "envoy.listener.tls_inspector"
      config: {}
    filter_chains:
    - filter_chain_match:
        transport_protocol: "tls"
    - filter_chain_match:
        transport_protocol: "tls"
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException,
                            "error adding listener '127.0.0.1:1234': multiple filter chains with "
                            "the same matching rules are defined");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsFilterChainWithoutTlsInspector) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - filter_chain_match:
        transport_protocol: "tls"
    - filter_chain_match:
        # empty
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS Inspector is automatically injected for filter chains with TLS requirements,
  // so make sure there is exactly 1 listener filter (and assume it's TLS Inspector).
  Network::ListenerConfig& listener = manager_->listeners().back().get();
  Network::FilterChainFactory& filterChainFactory = listener.filterChainFactory();
  Network::MockListenerFilterManager manager;
  EXPECT_CALL(manager, addAcceptFilter_(_))
      .WillOnce(Invoke([&](Network::ListenerFilterPtr&) -> void {}));
  EXPECT_TRUE(filterChainFactory.createListenerFilterChain(manager));
}

TEST_F(ListenerManagerImplWithRealFiltersTest, SniFilterChainWithoutTlsInspector) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - filter_chain_match:
        server_names: "example.com"
    - filter_chain_match:
        # empty
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS Inspector is automatically injected for filter chains with SNI requirements,
  // so make sure there is exactly 1 listener filter (and assume it's TLS Inspector).
  Network::ListenerConfig& listener = manager_->listeners().back().get();
  Network::FilterChainFactory& filterChainFactory = listener.filterChainFactory();
  Network::MockListenerFilterManager manager;
  EXPECT_CALL(manager, addAcceptFilter_(_))
      .WillOnce(Invoke([&](Network::ListenerFilterPtr&) -> void {}));
  EXPECT_TRUE(filterChainFactory.createListenerFilterChain(manager));
}

TEST_F(ListenerManagerImplWithRealFiltersTest, AlpnFilterChainWithoutTlsInspector) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - filter_chain_match:
        application_protocols: ["h2", "http/1.1"]
    - filter_chain_match:
        # empty
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // TLS Inspector is automatically injected for filter chains with ALPN requirements,
  // so make sure there is exactly 1 listener filter (and assume it's TLS Inspector).
  Network::ListenerConfig& listener = manager_->listeners().back().get();
  Network::FilterChainFactory& filterChainFactory = listener.filterChainFactory();
  Network::MockListenerFilterManager manager;
  EXPECT_CALL(manager, addAcceptFilter_(_))
      .WillOnce(Invoke([&](Network::ListenerFilterPtr&) -> void {}));
  EXPECT_TRUE(filterChainFactory.createListenerFilterChain(manager));
}

TEST_F(ListenerManagerImplWithRealFiltersTest, CustomTransportProtocolWithSniWithoutTlsInspector) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - filter_chain_match:
        server_names: "example.com"
        transport_protocol: "custom"
    - filter_chain_match:
        # empty
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  // Make sure there are no listener filters (i.e. no automatically injected TLS Inspector).
  Network::ListenerConfig& listener = manager_->listeners().back().get();
  Network::FilterChainFactory& filterChainFactory = listener.filterChainFactory();
  Network::MockListenerFilterManager manager;
  EXPECT_CALL(manager, addAcceptFilter_(_)).Times(0);
  EXPECT_TRUE(filterChainFactory.createListenerFilterChain(manager));
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsCertificateInline) {
  const std::string cert = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns3_chain.pem"));
  const std::string pkey = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns3_key.pem"));
  const std::string ca = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ca_cert.pem"));
  const std::string yaml = absl::StrCat(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { inline_string: ")EOF",
                                        absl::CEscape(cert), R"EOF(" }
              private_key: { inline_string: ")EOF",
                                        absl::CEscape(pkey), R"EOF(" }
          validation_context:
              trusted_ca: { inline_string: ")EOF",
                                        absl::CEscape(ca), R"EOF(" }
  )EOF");

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsCertificateChainInlinePrivateKeyFilename) {
  const std::string cert = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns3_chain.pem"));
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns3_key.pem" }
              certificate_chain: { inline_string: ")EOF",
                                                                    absl::CEscape(cert), R"EOF(" }
  )EOF"),
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsCertificateIncomplete) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns3_chain.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(
      manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true), EnvoyException,
      TestEnvironment::substitute(
          "Failed to load incomplete certificate from {{ test_rundir }}"
          "/test/extensions/transport_sockets/tls/test_data/san_dns3_chain.pem, ",
          Network::Address::IpVersion::v4));
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsCertificateInvalidCertificateChain) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { inline_string: "invalid" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns3_key.pem" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException, "Failed to load certificate chain from <inline>");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsCertificateInvalidIntermediateCA) {
  const std::string leaf = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns3_cert.pem"));
  const std::string yaml = TestEnvironment::substitute(
      absl::StrCat(
          R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { inline_string: ")EOF",
          absl::CEscape(leaf),
          R"EOF(\n-----BEGIN CERTIFICATE-----\nDEFINITELY_INVALID_CERTIFICATE\n-----END CERTIFICATE-----" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns3_key.pem" }
  )EOF"),
      Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException, "Failed to load certificate chain from <inline>");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsCertificateInvalidPrivateKey) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns3_chain.pem" }
              private_key: { inline_string: "invalid" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException, "Failed to load private key from <inline>");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, TlsCertificateInvalidTrustedCA) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns3_chain.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns3_key.pem" }
          validation_context:
              trusted_ca: { inline_string: "invalid" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException, "Failed to load trusted CA certificates from <inline>");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, Metadata) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    metadata: { filter_metadata: { com.bar.foo: { baz: test_value } } }
    filter_chains:
    - filter_chain_match:
      filters:
      - name: envoy.http_connection_manager
        config:
          stat_prefix: metadata_test
          route_config:
            virtual_hosts:
            - name: "some_virtual_host"
              domains: ["some.domain"]
              routes:
              - match: { prefix: "/" }
                route: { cluster: service_foo }
  )EOF",
                                                       Network::Address::IpVersion::v4);
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  auto context = dynamic_cast<Configuration::FactoryContext*>(&manager_->listeners().front().get());
  ASSERT_NE(nullptr, context);
  EXPECT_EQ("test_value",
            Config::Metadata::metadataValue(context->listenerMetadata(), "com.bar.foo", "baz")
                .string_value());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, OriginalDstFilter) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1111 }
    filter_chains: {}
    listener_filters:
    - name: "envoy.listener.original_dst"
      config: {}
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  Network::ListenerConfig& listener = manager_->listeners().back().get();

  Network::FilterChainFactory& filterChainFactory = listener.filterChainFactory();
  Network::MockListenerFilterManager manager;

  NiceMock<Network::MockListenerFilterCallbacks> callbacks;
  Network::AcceptedSocketImpl socket(std::make_unique<Network::IoSocketHandle>(),
                                     Network::Address::InstanceConstSharedPtr{
                                         new Network::Address::Ipv4Instance("127.0.0.1", 1234)},
                                     Network::Address::InstanceConstSharedPtr{
                                         new Network::Address::Ipv4Instance("127.0.0.1", 5678)});

  EXPECT_CALL(callbacks, socket()).WillOnce(Invoke([&]() -> Network::ConnectionSocket& {
    return socket;
  }));

  EXPECT_CALL(manager, addAcceptFilter_(_))
      .WillOnce(Invoke([&](Network::ListenerFilterPtr& filter) -> void {
        EXPECT_EQ(Network::FilterStatus::Continue, filter->onAccept(callbacks));
      }));

  EXPECT_TRUE(filterChainFactory.createListenerFilterChain(manager));
}

class OriginalDstTestFilter : public Extensions::ListenerFilters::OriginalDst::OriginalDstFilter {
  Network::Address::InstanceConstSharedPtr getOriginalDst(int) override {
    return Network::Address::InstanceConstSharedPtr{
        new Network::Address::Ipv4Instance("127.0.0.2", 2345)};
  }
};

TEST_F(ListenerManagerImplWithRealFiltersTest, OriginalDstTestFilter) {
  // Static scope required for the io_handle to be in scope for the lambda below
  // and for the final check at the end of this test.
  static int fd;
  fd = -1;
  // temporary io_handle to test result of socket creation
  Network::IoHandlePtr io_handle_tmp = std::make_unique<Network::IoSocketHandle>(0);
  EXPECT_CALL(*listener_factory_.socket_, ioHandle()).WillOnce(ReturnRef(*io_handle_tmp));

  class OriginalDstTestConfigFactory : public Configuration::NamedListenerFilterConfigFactory {
  public:
    // NamedListenerFilterConfigFactory
    Network::ListenerFilterFactoryCb
    createFilterFactoryFromProto(const Protobuf::Message&,
                                 Configuration::ListenerFactoryContext& context) override {
      auto option = std::make_unique<Network::MockSocketOption>();
      EXPECT_CALL(*option, setOption(_, envoy::api::v2::core::SocketOption::STATE_PREBIND))
          .WillOnce(Return(true));
      EXPECT_CALL(*option, setOption(_, envoy::api::v2::core::SocketOption::STATE_BOUND))
          .WillOnce(Invoke(
              [](Network::Socket& socket, envoy::api::v2::core::SocketOption::SocketState) -> bool {
                fd = socket.ioHandle().fd();
                return true;
              }));
      context.addListenSocketOption(std::move(option));
      return [](Network::ListenerFilterManager& filter_manager) -> void {
        filter_manager.addAcceptFilter(std::make_unique<OriginalDstTestFilter>());
      };
    }

    ProtobufTypes::MessagePtr createEmptyConfigProto() override {
      return std::make_unique<Envoy::ProtobufWkt::Empty>();
    }

    std::string name() override { return "test.listener.original_dst"; }
  };

  /**
   * Static registration for the original dst filter. @see RegisterFactory.
   */
  static Registry::RegisterFactory<OriginalDstTestConfigFactory,
                                   Configuration::NamedListenerFilterConfigFactory>
      registered_;

  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1111 }
    filter_chains: {}
    listener_filters:
    - name: "test.listener.original_dst"
      config: {}
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  Network::ListenerConfig& listener = manager_->listeners().back().get();

  Network::FilterChainFactory& filterChainFactory = listener.filterChainFactory();
  Network::MockListenerFilterManager manager;

  NiceMock<Network::MockListenerFilterCallbacks> callbacks;
  Network::AcceptedSocketImpl socket(
      std::make_unique<Network::IoSocketHandle>(),
      std::make_unique<Network::Address::Ipv4Instance>("127.0.0.1", 1234),
      std::make_unique<Network::Address::Ipv4Instance>("127.0.0.1", 5678));

  EXPECT_CALL(callbacks, socket()).WillOnce(Invoke([&]() -> Network::ConnectionSocket& {
    return socket;
  }));

  EXPECT_CALL(manager, addAcceptFilter_(_))
      .WillOnce(Invoke([&](Network::ListenerFilterPtr& filter) -> void {
        EXPECT_EQ(Network::FilterStatus::Continue, filter->onAccept(callbacks));
      }));

  EXPECT_TRUE(filterChainFactory.createListenerFilterChain(manager));
  EXPECT_TRUE(socket.localAddressRestored());
  EXPECT_EQ("127.0.0.2:2345", socket.localAddress()->asString());
  EXPECT_NE(fd, -1);
  io_handle_tmp->close();
}

TEST_F(ListenerManagerImplWithRealFiltersTest, OriginalDstTestFilterOptionFail) {
  class OriginalDstTestConfigFactory : public Configuration::NamedListenerFilterConfigFactory {
  public:
    // NamedListenerFilterConfigFactory
    Network::ListenerFilterFactoryCb
    createFilterFactoryFromProto(const Protobuf::Message&,
                                 Configuration::ListenerFactoryContext& context) override {
      auto option = std::make_unique<Network::MockSocketOption>();
      EXPECT_CALL(*option, setOption(_, envoy::api::v2::core::SocketOption::STATE_PREBIND))
          .WillOnce(Return(false));
      context.addListenSocketOption(std::move(option));
      return [](Network::ListenerFilterManager& filter_manager) -> void {
        filter_manager.addAcceptFilter(std::make_unique<OriginalDstTestFilter>());
      };
    }

    ProtobufTypes::MessagePtr createEmptyConfigProto() override {
      return std::make_unique<Envoy::ProtobufWkt::Empty>();
    }

    std::string name() override { return "testfail.listener.original_dst"; }
  };

  /**
   * Static registration for the original dst filter. @see RegisterFactory.
   */
  static Registry::RegisterFactory<OriginalDstTestConfigFactory,
                                   Configuration::NamedListenerFilterConfigFactory>
      registered_;

  const std::string yaml = TestEnvironment::substitute(R"EOF(
    name: "socketOptionFailListener"
    address:
      socket_address: { address: 127.0.0.1, port_value: 1111 }
    filter_chains: {}
    listener_filters:
    - name: "testfail.listener.original_dst"
      config: {}
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException,
                            "MockListenerComponentFactory: Setting socket options failed");
  EXPECT_EQ(0U, manager_->listeners().size());
}

class OriginalDstTestFilterIPv6
    : public Extensions::ListenerFilters::OriginalDst::OriginalDstFilter {
  Network::Address::InstanceConstSharedPtr getOriginalDst(int) override {
    return Network::Address::InstanceConstSharedPtr{
        new Network::Address::Ipv6Instance("1::2", 2345)};
  }
};

TEST_F(ListenerManagerImplWithRealFiltersTest, OriginalDstTestFilterIPv6) {
  // Static scope required for the io_handle to be in scope for the lambda below
  // and for the final check at the end of this test.
  static int fd;
  fd = -1;
  // temporary io_handle to test result of socket creation
  Network::IoHandlePtr io_handle_tmp = std::make_unique<Network::IoSocketHandle>(0);
  EXPECT_CALL(*listener_factory_.socket_, ioHandle()).WillOnce(ReturnRef(*io_handle_tmp));

  class OriginalDstTestConfigFactory : public Configuration::NamedListenerFilterConfigFactory {
  public:
    // NamedListenerFilterConfigFactory
    Network::ListenerFilterFactoryCb
    createFilterFactoryFromProto(const Protobuf::Message&,
                                 Configuration::ListenerFactoryContext& context) override {
      auto option = std::make_unique<Network::MockSocketOption>();
      EXPECT_CALL(*option, setOption(_, envoy::api::v2::core::SocketOption::STATE_PREBIND))
          .WillOnce(Return(true));
      EXPECT_CALL(*option, setOption(_, envoy::api::v2::core::SocketOption::STATE_BOUND))
          .WillOnce(Invoke(
              [](Network::Socket& socket, envoy::api::v2::core::SocketOption::SocketState) -> bool {
                fd = socket.ioHandle().fd();
                return true;
              }));
      context.addListenSocketOption(std::move(option));
      return [](Network::ListenerFilterManager& filter_manager) -> void {
        filter_manager.addAcceptFilter(std::make_unique<OriginalDstTestFilterIPv6>());
      };
    }

    ProtobufTypes::MessagePtr createEmptyConfigProto() override {
      return std::make_unique<Envoy::ProtobufWkt::Empty>();
    }

    std::string name() override { return "test.listener.original_dstipv6"; }
  };

  /**
   * Static registration for the original dst filter. @see RegisterFactory.
   */
  static Registry::RegisterFactory<OriginalDstTestConfigFactory,
                                   Configuration::NamedListenerFilterConfigFactory>
      registered_;

  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: ::0001, port_value: 1111 }
    filter_chains: {}
    listener_filters:
    - name: "test.listener.original_dstipv6"
      config: {}
  )EOF",
                                                       Network::Address::IpVersion::v6);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());

  Network::ListenerConfig& listener = manager_->listeners().back().get();

  Network::FilterChainFactory& filterChainFactory = listener.filterChainFactory();
  Network::MockListenerFilterManager manager;

  NiceMock<Network::MockListenerFilterCallbacks> callbacks;
  Network::AcceptedSocketImpl socket(
      std::make_unique<Network::IoSocketHandle>(),
      std::make_unique<Network::Address::Ipv6Instance>("::0001", 1234),
      std::make_unique<Network::Address::Ipv6Instance>("::0001", 5678));

  EXPECT_CALL(callbacks, socket()).WillOnce(Invoke([&]() -> Network::ConnectionSocket& {
    return socket;
  }));

  EXPECT_CALL(manager, addAcceptFilter_(_))
      .WillOnce(Invoke([&](Network::ListenerFilterPtr& filter) -> void {
        EXPECT_EQ(Network::FilterStatus::Continue, filter->onAccept(callbacks));
      }));

  EXPECT_TRUE(filterChainFactory.createListenerFilterChain(manager));
  EXPECT_TRUE(socket.localAddressRestored());
  EXPECT_EQ("[1::2]:2345", socket.localAddress()->asString());
  EXPECT_NE(fd, -1);
  io_handle_tmp->close();
}

TEST_F(ListenerManagerImplWithRealFiltersTest, OriginalDstTestFilterOptionFailIPv6) {
  class OriginalDstTestConfigFactory : public Configuration::NamedListenerFilterConfigFactory {
  public:
    // NamedListenerFilterConfigFactory
    Network::ListenerFilterFactoryCb
    createFilterFactoryFromProto(const Protobuf::Message&,
                                 Configuration::ListenerFactoryContext& context) override {
      auto option = std::make_unique<Network::MockSocketOption>();
      EXPECT_CALL(*option, setOption(_, envoy::api::v2::core::SocketOption::STATE_PREBIND))
          .WillOnce(Return(false));
      context.addListenSocketOption(std::move(option));
      return [](Network::ListenerFilterManager& filter_manager) -> void {
        filter_manager.addAcceptFilter(std::make_unique<OriginalDstTestFilterIPv6>());
      };
    }

    ProtobufTypes::MessagePtr createEmptyConfigProto() override {
      return std::make_unique<Envoy::ProtobufWkt::Empty>();
    }

    std::string name() override { return "testfail.listener.original_dstipv6"; }
  };

  /**
   * Static registration for the original dst filter. @see RegisterFactory.
   */
  static Registry::RegisterFactory<OriginalDstTestConfigFactory,
                                   Configuration::NamedListenerFilterConfigFactory>
      registered_;

  const std::string yaml = TestEnvironment::substitute(R"EOF(
    name: "socketOptionFailListener"
    address:
      socket_address: { address: ::0001, port_value: 1111 }
    filter_chains: {}
    listener_filters:
    - name: "testfail.listener.original_dstipv6"
      config: {}
  )EOF",
                                                       Network::Address::IpVersion::v6);

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException,
                            "MockListenerComponentFactory: Setting socket options failed");
  EXPECT_EQ(0U, manager_->listeners().size());
}

// Validate that when neither transparent nor freebind is not set in the
// Listener, we see no socket option set.
TEST_F(ListenerManagerImplWithRealFiltersTest, TransparentFreebindListenerDisabled) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    name: "TestListener"
    address:
      socket_address: { address: 127.0.0.1, port_value: 1111 }
    filter_chains:
    - filters:
  )EOF",
                                                       Network::Address::IpVersion::v4);
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true))
      .WillOnce(Invoke([&](Network::Address::InstanceConstSharedPtr, Network::Address::SocketType,
                           const Network::Socket::OptionsSharedPtr& options,
                           bool) -> Network::SocketSharedPtr {
        EXPECT_EQ(options, nullptr);
        return listener_factory_.socket_;
      }));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

// Validate that when transparent is set in the Listener, we see the socket option
// propagated to setsockopt(). This is as close to an end-to-end test as we have
// for this feature, due to the complexity of creating an integration test
// involving the network stack. We only test the IPv4 case here, as the logic
// around IPv4/IPv6 handling is tested generically in
// socket_option_impl_test.cc.
TEST_F(ListenerManagerImplWithRealFiltersTest, TransparentListenerEnabled) {
  auto listener = createIPv4Listener("TransparentListener");
  listener.mutable_transparent()->set_value(true);

  testSocketOption(listener, envoy::api::v2::core::SocketOption::STATE_PREBIND,
                   ENVOY_SOCKET_IP_TRANSPARENT, /* expected_value */ 1,
                   /* expected_num_options */ 2);
}

// Validate that when freebind is set in the Listener, we see the socket option
// propagated to setsockopt(). This is as close to an end-to-end test as we have
// for this feature, due to the complexity of creating an integration test
// involving the network stack. We only test the IPv4 case here, as the logic
// around IPv4/IPv6 handling is tested generically in
// socket_option_impl_test.cc.
TEST_F(ListenerManagerImplWithRealFiltersTest, FreebindListenerEnabled) {
  auto listener = createIPv4Listener("FreebindListener");
  listener.mutable_freebind()->set_value(true);

  testSocketOption(listener, envoy::api::v2::core::SocketOption::STATE_PREBIND,
                   ENVOY_SOCKET_IP_FREEBIND, /* expected_value */ 1);
}

// Validate that when tcp_fast_open_queue_length is set in the Listener, we see the socket option
// propagated to setsockopt(). This is as close to an end-to-end test as we have
// for this feature, due to the complexity of creating an integration test
// involving the network stack. We only test the IPv4 case here, as the logic
// around IPv4/IPv6 handling is tested generically in
// socket_option_impl_test.cc.
TEST_F(ListenerManagerImplWithRealFiltersTest, FastOpenListenerEnabled) {
  auto listener = createIPv4Listener("FastOpenListener");
  listener.mutable_tcp_fast_open_queue_length()->set_value(1);

  testSocketOption(listener, envoy::api::v2::core::SocketOption::STATE_LISTENING,
                   ENVOY_SOCKET_TCP_FASTOPEN, /* expected_value */ 1);
}

TEST_F(ListenerManagerImplWithRealFiltersTest, LiteralSockoptListenerEnabled) {
  NiceMock<Api::MockOsSysCalls> os_sys_calls;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> os_calls(&os_sys_calls);

  const envoy::api::v2::Listener listener = parseListenerFromV2Yaml(R"EOF(
    name: SockoptsListener
    address:
      socket_address: { address: 127.0.0.1, port_value: 1111 }
    filter_chains:
    - filters:
    socket_options: [
      # The socket goes through socket() and bind() but never listen(), so if we
      # ever saw (7, 8, 9) being applied it would cause a EXPECT_CALL failure.
      { level: 1, name: 2, int_value: 3, state: STATE_PREBIND },
      { level: 4, name: 5, int_value: 6, state: STATE_BOUND },
      { level: 7, name: 8, int_value: 9, state: STATE_LISTENING },
    ]
  )EOF");

  expectCreateListenSocket(envoy::api::v2::core::SocketOption::STATE_PREBIND,
                           /* expected_num_options */ 3);
  expectSetsockopt(os_sys_calls,
                   /* expected_sockopt_level */ 1,
                   /* expected_sockopt_name */ 2,
                   /* expected_value */ 3);
  expectSetsockopt(os_sys_calls,
                   /* expected_sockopt_level */ 4,
                   /* expected_sockopt_name */ 5,
                   /* expected_value */ 6);
  manager_->addOrUpdateListener(listener, "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

// Set the resolver to the default IP resolver. The address resolver logic is unit tested in
// resolver_impl_test.cc.
TEST_F(ListenerManagerImplWithRealFiltersTest, AddressResolver) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    name: AddressResolverdListener
    address:
      socket_address: { address: 127.0.0.1, port_value: 1111, resolver_name: envoy.mock.resolver }
    filter_chains:
    - filters:
  )EOF",
                                                       Network::Address::IpVersion::v4);

  NiceMock<Network::MockAddressResolver> mock_resolver;
  EXPECT_CALL(mock_resolver, resolve(_))
      .WillOnce(Return(Network::Utility::parseInternetAddress("127.0.0.1", 1111, false)));

  Registry::InjectFactory<Network::Address::Resolver> register_resolver(mock_resolver);

  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, CRLFilename) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
          validation_context:
            trusted_ca: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ca_cert.pem" }
            crl: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ca_cert.crl" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, CRLInline) {
  const std::string crl = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ca_cert.crl"));
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
          validation_context:
            trusted_ca: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ca_cert.pem" }
            crl: { inline_string: ")EOF",
                                                                    absl::CEscape(crl), R"EOF(" }
  )EOF"),
                                                       Network::Address::IpVersion::v4);

  EXPECT_CALL(server_.random_, uuid());
  EXPECT_CALL(listener_factory_, createListenSocket(_, _, _, true));
  manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true);
  EXPECT_EQ(1U, manager_->listeners().size());
}

TEST_F(ListenerManagerImplWithRealFiltersTest, InvalidCRLInline) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
          validation_context:
            trusted_ca: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ca_cert.pem" }
            crl: { inline_string: "-----BEGIN X509 CRL-----\nTOTALLY_NOT_A_CRL_HERE\n-----END X509 CRL-----\n" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException, "Failed to load CRL from <inline>");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, CRLWithNoCA) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
          validation_context:
            crl: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ca_cert.crl" }
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_REGEX(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                          EnvoyException, "^Failed to load CRL from .* without trusted CA$");
}

TEST_F(ListenerManagerImplWithRealFiltersTest, VerifySanWithNoCA) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
          validation_context:
            verify_subject_alt_name: "spiffe://lyft.com/testclient"
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException,
                            "SAN-based verification of peer certificates without trusted CA "
                            "is insecure and not allowed");
}

// Disabling certificate expiration checks only makes sense with a trusted CA.
TEST_F(ListenerManagerImplWithRealFiltersTest, VerifyIgnoreExpirationWithNoCA) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }
          validation_context:
            allow_expired_certificate: true
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_THROW_WITH_MESSAGE(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true),
                            EnvoyException,
                            "Certificate validity period is always ignored without trusted CA");
}

// Verify that with a CA, expired certificates are allowed.
TEST_F(ListenerManagerImplWithRealFiltersTest, VerifyIgnoreExpirationWithCA) {
  const std::string yaml = TestEnvironment::substitute(R"EOF(
    address:
      socket_address: { address: 127.0.0.1, port_value: 1234 }
    filter_chains:
    - tls_context:
        common_tls_context:
          tls_certificates:
            - certificate_chain: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_cert.pem" }
              private_key: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/san_dns_key.pem" }

          validation_context:
            trusted_ca: { filename: "{{ test_rundir }}/test/extensions/transport_sockets/tls/test_data/ca_cert.pem" }
            allow_expired_certificate: true
  )EOF",
                                                       Network::Address::IpVersion::v4);

  EXPECT_NO_THROW(manager_->addOrUpdateListener(parseListenerFromV2Yaml(yaml), "", true));
}

} // namespace Server
} // namespace Envoy
