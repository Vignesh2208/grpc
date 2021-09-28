// Copyright 2021 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string>
#include <thread>  // NOLINT
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <grpc++/grpc++.h>
#include "src/core/lib/gpr/env.h"
#include "src/core/lib/gpr/tmpfile.h"
#include "src/proto/grpc/testing/echo.grpc.pb.h"
#include "test/core/util/test_config.h"
#include "test/core/util/tls_utils.h"

#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/security/tls_credentials_options.h>
#include <grpcpp/support/channel_arguments.h>

#include <memory>

#include "src/cpp/client/secure_credentials.h"

extern "C" {
#include <openssl/ssl.h>
}

#if OPENSSL_VERSION_NUMBER >= 0x10100000 && !defined(LIBRESSL_VERSION_NUMBER)
#define TLS_KEY_LOGGING_AVAILABLE
#endif

#define CA_CERT_PATH "src/core/tsi/test_creds/ca.pem"
#define SERVER_KEY_PATH "src/core/tsi/test_creds/server0.key"
#define SERVER_CERT_PATH "src/core/tsi/test_creds/server0.pem"
#define CLIENT_KEY_PATH "src/core/tsi/test_creds/client.key"
#define CLIENT_CERT_PATH "src/core/tsi/test_creds/client.pem"

#define NUM_REQUESTS_PER_CHANNEL 5

using ::grpc::experimental::FileWatcherCertificateProvider;
using ::grpc::experimental::TlsChannelCredentialsOptions;
using ::grpc::experimental::TlsSessionKeyLoggerConfig;
using ::grpc::experimental::TlsServerCredentialsOptions;

namespace grpc {
namespace testing {
namespace {

class EchoServer final : public EchoTestService::Service {
  ::grpc::Status Echo(::grpc::ServerContext* /*context*/,
                      const EchoRequest* request,
                      EchoResponse* response) override {
    if (request->param().expected_error().code() == 0) {
      response->set_message(request->message());
      return ::grpc::Status::OK;
    } else {
      return ::grpc::Status(static_cast<::grpc::StatusCode>(
                                request->param().expected_error().code()),
                            "");
    }
  }
};

// A no-op server authorization check.
class ServerAuthzCheck
    : public ::grpc::experimental::TlsServerAuthorizationCheckInterface {
 public:
  int Schedule(
      ::grpc::experimental::TlsServerAuthorizationCheckArg* arg) override {
    if (arg != nullptr) {
      arg->set_status(GRPC_STATUS_OK);
      arg->set_success(true);
    }
    return 0;
  }
};

class TestScenario {
 public:
  TestScenario(int num_listening_ports, bool share_tls_key_log_file,
               bool enable_tls_key_logging)
      : num_listening_ports_(num_listening_ports),
        share_tls_key_log_file_(share_tls_key_log_file),
        enable_tls_key_logging_(enable_tls_key_logging) {}
  std::string AsString() const {
    return absl::StrCat(
             "TestScenario{num_listening_ports=", num_listening_ports_,
             ", share_tls_key_log_file=",
             (share_tls_key_log_file_ ? "true" : "false"),
             ", enable_tls_key_logging=",
             (enable_tls_key_logging_ ? "true" : "false"), "'}");
  }

  int num_listening_ports() const {
    return num_listening_ports_;
  }

  bool share_tls_key_log_file() const {
    return share_tls_key_log_file_;
  }

  bool enable_tls_key_logging() const {
    return enable_tls_key_logging_;
  }
 private:
  int num_listening_ports_;
  bool share_tls_key_log_file_;
  bool enable_tls_key_logging_;
};

int CountOccurancesInFileContents(std::string file_contents,
                                  std::string search_string) {
  int occurrences = 0;
  std::string::size_type pos = 0;
  while ((pos = file_contents.find(search_string, pos)) != std::string::npos) {
    ++occurrences;
    pos += search_string.length();
  }
  return occurrences;
}

class TlsKeyLoggingEnd2EndTest : public ::testing::TestWithParam<TestScenario> {
 protected:
  TlsKeyLoggingEnd2EndTest() {
    gpr_log(GPR_INFO, "%s\n", GetParam().AsString().c_str());
  }

  std::string CreateTmpFile() {
    char* name = nullptr;
    FILE* file_descriptor = gpr_tmpfile("GrpcTlsKeyLoggerTest", &name);
    GPR_ASSERT(fclose(file_descriptor) == 0);
    GPR_ASSERT(file_descriptor != nullptr);
    GPR_ASSERT(name != nullptr);
    std::string name_to_return = name;
    gpr_free(name);
    return name_to_return;
  }

  void SetUp() override {

    ::grpc::ServerBuilder builder;
    ::grpc::ChannelArguments args;
    args.SetSslTargetNameOverride("foo.test.google.com.au");

    if (GetParam().num_listening_ports() > 0) {
      ports_.reserve(GetParam().num_listening_ports());
    }

    std::string shared_key_log_file_server;
    std::string shared_key_log_file_channel;

    if (GetParam().share_tls_key_log_file()) {
      shared_key_log_file_server = CreateTmpFile();
      shared_key_log_file_channel = CreateTmpFile();
    }

    auto server_certificate_provider =
        std::make_shared<FileWatcherCertificateProvider>(
            SERVER_KEY_PATH, SERVER_CERT_PATH, CA_CERT_PATH, 1);

    auto channel_certificate_provider =
        std::make_shared<FileWatcherCertificateProvider>(
            CLIENT_KEY_PATH, CLIENT_CERT_PATH, CA_CERT_PATH, 1);

    auth_check_ = std::make_shared<
        ::grpc::experimental::TlsServerAuthorizationCheckConfig>(
        std::make_shared<ServerAuthzCheck>());

    for (int i = 0; i < GetParam().num_listening_ports(); i++) {
      // Configure tls credential options for each port
      TlsSessionKeyLoggerConfig server_port_tls_key_log_config;
      TlsServerCredentialsOptions server_creds_options(
          server_certificate_provider);
      server_creds_options.set_cert_request_type(
          GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY);
      server_creds_options.watch_identity_key_cert_pairs();
      server_creds_options.watch_root_certs();

      // Set a separate ssl key log file for each port if not shared
      if (GetParam().share_tls_key_log_file()) {
        tmp_server_tls_key_log_file_by_port_.push_back(
            shared_key_log_file_server);
      } else {
        tmp_server_tls_key_log_file_by_port_.push_back(CreateTmpFile());
      }

      if (GetParam().enable_tls_key_logging()) {
        server_port_tls_key_log_config.set_tls_session_key_log_file_path(
            tmp_server_tls_key_log_file_by_port_[i]);
        server_creds_options.set_tls_session_key_log_config(
            server_port_tls_key_log_config);
      }

      builder.AddListeningPort(
          "0.0.0.0:0",
          ::grpc::experimental::TlsServerCredentials(server_creds_options),
          &ports_[i]);
    }

    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(nullptr, server_);
    server_thread_ =
        std::thread(&TlsKeyLoggingEnd2EndTest::RunServerLoop, this);

    for (int i = 0; i < GetParam().num_listening_ports(); i++) {
      ASSERT_NE(0, ports_[i]);
      server_addresses_.push_back(absl::StrCat("localhost:", ports_[i]));

      // Configure tls credential options for each stub. Each stub connects to
      // a separate port on the server.
      TlsSessionKeyLoggerConfig stub_tls_key_log_config;
      TlsChannelCredentialsOptions channel_creds_options;
      channel_creds_options.set_certificate_provider(
          channel_certificate_provider);
      channel_creds_options.set_server_verification_option(
          GRPC_TLS_SKIP_HOSTNAME_VERIFICATION);
      channel_creds_options.watch_identity_key_cert_pairs();
      channel_creds_options.watch_root_certs();
      channel_creds_options.set_server_authorization_check_config(auth_check_);

      // Set a separate ssl key log file for each port if not shared.
      if (GetParam().share_tls_key_log_file()) {
        tmp_stub_tls_key_log_file_.push_back(shared_key_log_file_channel);
      } else {
        tmp_stub_tls_key_log_file_.push_back(CreateTmpFile());
      }

      if (GetParam().enable_tls_key_logging()) {
        stub_tls_key_log_config.set_tls_session_key_log_file_path(
            tmp_stub_tls_key_log_file_[i]);
        channel_creds_options.set_tls_session_key_log_config(
            stub_tls_key_log_config);
      }

      stubs_.push_back(EchoTestService::NewStub(::grpc::CreateCustomChannel(
          server_addresses_[i],
          ::grpc::experimental::TlsCredentials(channel_creds_options), args)));
    }
  }

  void TearDown() override {
    server_->Shutdown();
    server_thread_.join();

    // Remove all created files.
    for (int i = 0; i < GetParam().num_listening_ports(); i++) {
      remove(tmp_stub_tls_key_log_file_[i].c_str());
      remove(tmp_server_tls_key_log_file_by_port_[i].c_str());
      if (GetParam().share_tls_key_log_file()) {
        break;
      }
    }
  }

  void RunServerLoop() { server_->Wait(); }

  const std::string client_method_name_ = "grpc.testing.EchoTestService/Echo";
  const std::string server_method_name_ = "grpc.testing.EchoTestService/Echo";

  std::vector<int> ports_;
  std::vector<std::string> tmp_server_tls_key_log_file_by_port_;
  std::vector<std::string> tmp_stub_tls_key_log_file_;
  std::vector<std::string> server_addresses_;
  std::vector<std::unique_ptr<EchoTestService::Stub>> stubs_;
  EchoServer service_;
  std::unique_ptr<::grpc::Server> server_;
  std::shared_ptr<::grpc::experimental::TlsServerAuthorizationCheckConfig>
      auth_check_;
  std::thread server_thread_;
};

TEST_P(TlsKeyLoggingEnd2EndTest, KeyLogging) {
  // Cover all valid statuses.
  for (int i = 0; i <= NUM_REQUESTS_PER_CHANNEL; ++i) {
    for (int j = 0; j < GetParam().num_listening_ports(); ++j) {
      EchoRequest request;
      request.set_message("foo");
      request.mutable_param()->mutable_expected_error()->set_code(0);
      EchoResponse response;
      ::grpc::ClientContext context;
      ::grpc::Status status = stubs_[j]->Echo(&context, request, &response);
      EXPECT_TRUE(status.ok());
    }
  }

  for (int i = 0; i < GetParam().num_listening_ports(); i++) {
    std::string server_key_log = ::grpc_core::testing::GetFileContents(
        tmp_server_tls_key_log_file_by_port_[i].c_str());
    std::string channel_key_log = ::grpc_core::testing::GetFileContents(
        tmp_stub_tls_key_log_file_[i].c_str());

    if (!GetParam().enable_tls_key_logging()) {
      EXPECT_THAT(server_key_log, ::testing::IsEmpty());
      EXPECT_THAT(channel_key_log, ::testing::IsEmpty());
    }

#ifdef TLS_KEY_LOGGING_AVAILABLE
    EXPECT_THAT(server_key_log, ::testing::StrEq(channel_key_log));

    if (GetParam().share_tls_key_log_file() && GetParam().enable_tls_key_logging()) {
      EXPECT_EQ(CountOccurancesInFileContents(
                    server_key_log, "CLIENT_HANDSHAKE_TRAFFIC_SECRET"),
                GetParam().num_listening_ports());
      EXPECT_EQ(CountOccurancesInFileContents(
                    server_key_log, "SERVER_HANDSHAKE_TRAFFIC_SECRET"),
                GetParam().num_listening_ports());
      EXPECT_EQ(CountOccurancesInFileContents(server_key_log,
                                              "CLIENT_TRAFFIC_SECRET_0"),
                GetParam().num_listening_ports());
      EXPECT_EQ(CountOccurancesInFileContents(server_key_log,
                                              "SERVER_TRAFFIC_SECRET_0"),
                GetParam().num_listening_ports());
      EXPECT_EQ(
          CountOccurancesInFileContents(server_key_log, "EXPORTER_SECRET"),
          GetParam().num_listening_ports());
    } else if (GetParam().enable_tls_key_logging()) {
      EXPECT_EQ(CountOccurancesInFileContents(
                    server_key_log, "CLIENT_HANDSHAKE_TRAFFIC_SECRET"),
                1);
      EXPECT_EQ(CountOccurancesInFileContents(
                    server_key_log, "SERVER_HANDSHAKE_TRAFFIC_SECRET"),
                1);
      EXPECT_EQ(CountOccurancesInFileContents(server_key_log,
                                              "CLIENT_TRAFFIC_SECRET_0"),
                1);
      EXPECT_EQ(CountOccurancesInFileContents(server_key_log,
                                              "SERVER_TRAFFIC_SECRET_0"),
                1);
      EXPECT_EQ(
          CountOccurancesInFileContents(server_key_log, "EXPORTER_SECRET"), 1);
    }
#else
    // If TLS Key logging is not available, the files should be empty.
    if (GetParam().enable_tls_key_logging()) {
      EXPECT_THAT(server_key_log, ::testing::IsEmpty());
      EXPECT_THAT(channel_key_log, ::testing::IsEmpty());
    }
#endif

    if (GetParam().share_tls_key_log_file()) {
      break;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(TlsKeyLogging, TlsKeyLoggingEnd2EndTest,
                         ::testing::ValuesIn({TestScenario(5, false, true),
                                              TestScenario(5, true, true),
                                              TestScenario(5, true, false),
                                              TestScenario(5, false, false)}));


}  // namespace
}  // namespace testing
}  // namespace grpc

int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
