/*
 *
 * Copyright 2015-2016, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <unistd.h>
#include <cinttypes>
#include <fstream>
#include <memory>

#include <grpc++/channel.h>
#include <grpc++/client_context.h>
#include <grpc++/security/credentials.h>
#include <grpc/grpc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/useful.h>

#include "src/core/lib/transport/byte_stream.h"
#include "src/proto/grpc/testing/empty.grpc.pb.h"
#include "src/proto/grpc/testing/messages.grpc.pb.h"
#include "src/proto/grpc/testing/test.grpc.pb.h"
#include "test/cpp/interop/client_helper.h"
#include "test/cpp/interop/interop_client.h"

namespace grpc {
namespace testing {

namespace {
// The same value is defined by the Java client.
const std::vector<int> request_stream_sizes = {27182, 8, 1828, 45904};
const std::vector<int> response_stream_sizes = {31415, 9, 2653, 58979};
const int kNumResponseMessages = 2000;
const int kResponseMessageSize = 1030;
const int kReceiveDelayMilliSeconds = 20;
const int kLargeRequestSize = 271828;
const int kLargeResponseSize = 314159;

void NoopChecks(const InteropClientContextInspector& inspector,
                const SimpleRequest* request, const SimpleResponse* response) {}

void UnaryCompressionChecks(const InteropClientContextInspector& inspector,
                            const SimpleRequest* request,
                            const SimpleResponse* response) {
  const grpc_compression_algorithm received_compression =
      inspector.GetCallCompressionAlgorithm();
  if (request->response_compressed().value()) {
    if (received_compression == GRPC_COMPRESS_NONE) {
      // Requested some compression, got NONE. This is an error.
      gpr_log(GPR_ERROR,
              "Failure: Requested compression but got uncompressed response "
              "from server.");
      abort();
    }
    GPR_ASSERT(inspector.GetMessageFlags() & GRPC_WRITE_INTERNAL_COMPRESS);
  } else {
    // Didn't request compression -> make sure the response is uncompressed
    GPR_ASSERT(!(inspector.GetMessageFlags() & GRPC_WRITE_INTERNAL_COMPRESS));
  }
}
}  // namespace

InteropClient::ServiceStub::ServiceStub(std::shared_ptr<Channel> channel,
                                        bool new_stub_every_call)
    : channel_(channel), new_stub_every_call_(new_stub_every_call) {
  // If new_stub_every_call is false, then this is our chance to initialize
  // stub_. (see Get())
  if (!new_stub_every_call) {
    stub_ = TestService::NewStub(channel);
  }
}

TestService::Stub* InteropClient::ServiceStub::Get() {
  if (new_stub_every_call_) {
    stub_ = TestService::NewStub(channel_);
  }

  return stub_.get();
}

void InteropClient::ServiceStub::Reset(std::shared_ptr<Channel> channel) {
  channel_ = channel;

  // Update stub_ as well. Note: If new_stub_every_call_ is true, we can reset
  // the stub_ since the next call to Get() will create a new stub
  if (new_stub_every_call_) {
    stub_.reset();
  } else {
    stub_ = TestService::NewStub(channel);
  }
}

void InteropClient::Reset(std::shared_ptr<Channel> channel) {
  serviceStub_.Reset(channel);
}

InteropClient::InteropClient(std::shared_ptr<Channel> channel,
                             bool new_stub_every_test_case,
                             bool do_not_abort_on_transient_failures)
    : serviceStub_(channel, new_stub_every_test_case),
      do_not_abort_on_transient_failures_(do_not_abort_on_transient_failures) {}

bool InteropClient::AssertStatusOk(const Status& s) {
  if (s.ok()) {
    return true;
  }

  // Note: At this point, s.error_code is definitely not StatusCode::OK (we
  // already checked for s.ok() above). So, the following will call abort()
  // (unless s.error_code() corresponds to a transient failure and
  // 'do_not_abort_on_transient_failures' is true)
  return AssertStatusCode(s, StatusCode::OK);
}

bool InteropClient::AssertStatusCode(const Status& s,
                                     StatusCode expected_code) {
  if (s.error_code() == expected_code) {
    return true;
  }

  gpr_log(GPR_ERROR, "Error status code: %d (expected: %d), message: %s",
          s.error_code(), expected_code, s.error_message().c_str());

  // In case of transient transient/retryable failures (like a broken
  // connection) we may or may not abort (see TransientFailureOrAbort())
  if (s.error_code() == grpc::StatusCode::UNAVAILABLE) {
    return TransientFailureOrAbort();
  }

  abort();
}

bool InteropClient::DoEmpty() {
  gpr_log(GPR_DEBUG, "Sending an empty rpc...");

  Empty request = Empty::default_instance();
  Empty response = Empty::default_instance();
  ClientContext context;

  Status s = serviceStub_.Get()->EmptyCall(&context, request, &response);

  if (!AssertStatusOk(s)) {
    return false;
  }

  gpr_log(GPR_DEBUG, "Empty rpc done.");
  return true;
}

bool InteropClient::PerformLargeUnary(SimpleRequest* request,
                                      SimpleResponse* response) {
  return PerformLargeUnary(request, response, NoopChecks);
}

bool InteropClient::PerformLargeUnary(SimpleRequest* request,
                                      SimpleResponse* response,
                                      CheckerFn custom_checks_fn) {
  ClientContext context;
  InteropClientContextInspector inspector(context);
  request->set_response_size(kLargeResponseSize);
  grpc::string payload(kLargeRequestSize, '\0');
  request->mutable_payload()->set_body(payload.c_str(), kLargeRequestSize);
  if (request->has_expect_compressed()) {
    if (request->expect_compressed().value()) {
      context.set_compression_algorithm(GRPC_COMPRESS_GZIP);
    } else {
      context.set_compression_algorithm(GRPC_COMPRESS_NONE);
    }
  }

  Status s = serviceStub_.Get()->UnaryCall(&context, *request, response);
  if (!AssertStatusOk(s)) {
    return false;
  }

  custom_checks_fn(inspector, request, response);

  // Payload related checks.
  GPR_ASSERT(response->payload().body() ==
             grpc::string(kLargeResponseSize, '\0'));
  return true;
}

bool InteropClient::DoComputeEngineCreds(
    const grpc::string& default_service_account,
    const grpc::string& oauth_scope) {
  gpr_log(GPR_DEBUG,
          "Sending a large unary rpc with compute engine credentials ...");
  SimpleRequest request;
  SimpleResponse response;
  request.set_fill_username(true);
  request.set_fill_oauth_scope(true);

  if (!PerformLargeUnary(&request, &response)) {
    return false;
  }

  gpr_log(GPR_DEBUG, "Got username %s", response.username().c_str());
  gpr_log(GPR_DEBUG, "Got oauth_scope %s", response.oauth_scope().c_str());
  GPR_ASSERT(!response.username().empty());
  GPR_ASSERT(response.username().c_str() == default_service_account);
  GPR_ASSERT(!response.oauth_scope().empty());
  const char* oauth_scope_str = response.oauth_scope().c_str();
  GPR_ASSERT(oauth_scope.find(oauth_scope_str) != grpc::string::npos);
  gpr_log(GPR_DEBUG, "Large unary with compute engine creds done.");
  return true;
}

bool InteropClient::DoOauth2AuthToken(const grpc::string& username,
                                      const grpc::string& oauth_scope) {
  gpr_log(GPR_DEBUG,
          "Sending a unary rpc with raw oauth2 access token credentials ...");
  SimpleRequest request;
  SimpleResponse response;
  request.set_fill_username(true);
  request.set_fill_oauth_scope(true);

  ClientContext context;

  Status s = serviceStub_.Get()->UnaryCall(&context, request, &response);

  if (!AssertStatusOk(s)) {
    return false;
  }

  GPR_ASSERT(!response.username().empty());
  GPR_ASSERT(!response.oauth_scope().empty());
  GPR_ASSERT(username == response.username());
  const char* oauth_scope_str = response.oauth_scope().c_str();
  GPR_ASSERT(oauth_scope.find(oauth_scope_str) != grpc::string::npos);
  gpr_log(GPR_DEBUG, "Unary with oauth2 access token credentials done.");
  return true;
}

bool InteropClient::DoPerRpcCreds(const grpc::string& json_key) {
  gpr_log(GPR_DEBUG, "Sending a unary rpc with per-rpc JWT access token ...");
  SimpleRequest request;
  SimpleResponse response;
  request.set_fill_username(true);

  ClientContext context;
  std::chrono::seconds token_lifetime = std::chrono::hours(1);
  std::shared_ptr<CallCredentials> creds =
      ServiceAccountJWTAccessCredentials(json_key, token_lifetime.count());

  context.set_credentials(creds);

  Status s = serviceStub_.Get()->UnaryCall(&context, request, &response);

  if (!AssertStatusOk(s)) {
    return false;
  }

  GPR_ASSERT(!response.username().empty());
  GPR_ASSERT(json_key.find(response.username()) != grpc::string::npos);
  gpr_log(GPR_DEBUG, "Unary with per-rpc JWT access token done.");
  return true;
}

bool InteropClient::DoJwtTokenCreds(const grpc::string& username) {
  gpr_log(GPR_DEBUG,
          "Sending a large unary rpc with JWT token credentials ...");
  SimpleRequest request;
  SimpleResponse response;
  request.set_fill_username(true);

  if (!PerformLargeUnary(&request, &response)) {
    return false;
  }

  GPR_ASSERT(!response.username().empty());
  GPR_ASSERT(username.find(response.username()) != grpc::string::npos);
  gpr_log(GPR_DEBUG, "Large unary with JWT token creds done.");
  return true;
}

bool InteropClient::DoLargeUnary() {
  gpr_log(GPR_DEBUG, "Sending a large unary rpc...");
  SimpleRequest request;
  SimpleResponse response;
  if (!PerformLargeUnary(&request, &response)) {
    return false;
  }
  gpr_log(GPR_DEBUG, "Large unary done.");
  return true;
}

bool InteropClient::DoClientCompressedUnary() {
  // Probing for compression-checks support.
  ClientContext probe_context;
  SimpleRequest probe_req;
  SimpleResponse probe_res;

  probe_context.set_compression_algorithm(GRPC_COMPRESS_NONE);
  probe_req.mutable_expect_compressed()->set_value(true);  // lies!

  probe_req.set_response_size(kLargeResponseSize);
  probe_req.mutable_payload()->set_body(grpc::string(kLargeRequestSize, '\0'));

  gpr_log(GPR_DEBUG, "Sending probe for compressed unary request.");
  const Status s =
      serviceStub_.Get()->UnaryCall(&probe_context, probe_req, &probe_res);
  if (s.error_code() != grpc::StatusCode::INVALID_ARGUMENT) {
    // The server isn't able to evaluate incoming compression, making the rest
    // of this test moot.
    gpr_log(GPR_DEBUG, "Compressed unary request probe failed");
    return false;
  }
  gpr_log(GPR_DEBUG, "Compressed unary request probe succeeded. Proceeding.");

  const std::vector<bool> compressions = {true, false};
  for (size_t i = 0; i < compressions.size(); i++) {
    char* log_suffix;
    gpr_asprintf(&log_suffix, "(compression=%s)",
                 compressions[i] ? "true" : "false");

    gpr_log(GPR_DEBUG, "Sending compressed unary request %s.", log_suffix);
    SimpleRequest request;
    SimpleResponse response;
    request.mutable_expect_compressed()->set_value(compressions[i]);
    if (!PerformLargeUnary(&request, &response, UnaryCompressionChecks)) {
      gpr_log(GPR_ERROR, "Compressed unary request failed %s", log_suffix);
      gpr_free(log_suffix);
      return false;
    }

    gpr_log(GPR_DEBUG, "Compressed unary request failed %s", log_suffix);
    gpr_free(log_suffix);
  }

  return true;
}

bool InteropClient::DoServerCompressedUnary() {
  const std::vector<bool> compressions = {true, false};
  for (size_t i = 0; i < compressions.size(); i++) {
    char* log_suffix;
    gpr_asprintf(&log_suffix, "(compression=%s)",
                 compressions[i] ? "true" : "false");

    gpr_log(GPR_DEBUG, "Sending unary request for compressed response %s.",
            log_suffix);
    SimpleRequest request;
    SimpleResponse response;
    request.mutable_response_compressed()->set_value(compressions[i]);

    if (!PerformLargeUnary(&request, &response, UnaryCompressionChecks)) {
      gpr_log(GPR_ERROR, "Request for compressed unary failed %s", log_suffix);
      gpr_free(log_suffix);
      return false;
    }

    gpr_log(GPR_DEBUG, "Request for compressed unary failed %s", log_suffix);
    gpr_free(log_suffix);
  }

  return true;
}

// Either abort() (unless do_not_abort_on_transient_failures_ is true) or return
// false
bool InteropClient::TransientFailureOrAbort() {
  if (do_not_abort_on_transient_failures_) {
    return false;
  }

  abort();
}

bool InteropClient::DoRequestStreaming() {
  gpr_log(GPR_DEBUG, "Sending request steaming rpc ...");

  ClientContext context;
  StreamingInputCallRequest request;
  StreamingInputCallResponse response;

  std::unique_ptr<ClientWriter<StreamingInputCallRequest>> stream(
      serviceStub_.Get()->StreamingInputCall(&context, &response));

  int aggregated_payload_size = 0;
  for (size_t i = 0; i < request_stream_sizes.size(); ++i) {
    Payload* payload = request.mutable_payload();
    payload->set_body(grpc::string(request_stream_sizes[i], '\0'));
    if (!stream->Write(request)) {
      gpr_log(GPR_ERROR, "DoRequestStreaming(): stream->Write() failed");
      return TransientFailureOrAbort();
    }
    aggregated_payload_size += request_stream_sizes[i];
  }
  GPR_ASSERT(stream->WritesDone());

  Status s = stream->Finish();
  if (!AssertStatusOk(s)) {
    return false;
  }

  GPR_ASSERT(response.aggregated_payload_size() == aggregated_payload_size);
  return true;
}

bool InteropClient::DoResponseStreaming() {
  gpr_log(GPR_DEBUG, "Receiving response streaming rpc ...");

  ClientContext context;
  StreamingOutputCallRequest request;
  for (unsigned int i = 0; i < response_stream_sizes.size(); ++i) {
    ResponseParameters* response_parameter = request.add_response_parameters();
    response_parameter->set_size(response_stream_sizes[i]);
  }
  StreamingOutputCallResponse response;
  std::unique_ptr<ClientReader<StreamingOutputCallResponse>> stream(
      serviceStub_.Get()->StreamingOutputCall(&context, request));

  unsigned int i = 0;
  while (stream->Read(&response)) {
    GPR_ASSERT(response.payload().body() ==
               grpc::string(response_stream_sizes[i], '\0'));
    ++i;
  }

  if (i < response_stream_sizes.size()) {
    // stream->Read() failed before reading all the expected messages. This is
    // most likely due to connection failure.
    gpr_log(GPR_ERROR,
            "DoResponseStreaming(): Read fewer streams (%d) than "
            "response_stream_sizes.size() (%" PRIuPTR ")",
            i, response_stream_sizes.size());
    return TransientFailureOrAbort();
  }

  Status s = stream->Finish();
  if (!AssertStatusOk(s)) {
    return false;
  }

  gpr_log(GPR_DEBUG, "Response streaming done.");
  return true;
}

bool InteropClient::DoClientCompressedStreaming() {
  // Probing for compression-checks support.
  ClientContext probe_context;
  StreamingInputCallRequest probe_req;
  StreamingInputCallResponse probe_res;

  probe_context.set_compression_algorithm(GRPC_COMPRESS_NONE);
  probe_req.mutable_expect_compressed()->set_value(true);  // lies!
  probe_req.mutable_payload()->set_body(grpc::string(27182, '\0'));

  gpr_log(GPR_DEBUG, "Sending probe for compressed streaming request.");

  std::unique_ptr<ClientWriter<StreamingInputCallRequest>> probe_stream(
      serviceStub_.Get()->StreamingInputCall(&probe_context, &probe_res));

  if (!probe_stream->Write(probe_req)) {
    gpr_log(GPR_ERROR, "%s(): stream->Write() failed", __func__);
    return TransientFailureOrAbort();
  }
  Status s = probe_stream->Finish();
  if (s.error_code() != grpc::StatusCode::INVALID_ARGUMENT) {
    // The server isn't able to evaluate incoming compression, making the rest
    // of this test moot.
    gpr_log(GPR_DEBUG, "Compressed streaming request probe failed");
    return false;
  }
  gpr_log(GPR_DEBUG,
          "Compressed streaming request probe succeeded. Proceeding.");

  ClientContext context;
  StreamingInputCallRequest request;
  StreamingInputCallResponse response;

  context.set_compression_algorithm(GRPC_COMPRESS_GZIP);
  std::unique_ptr<ClientWriter<StreamingInputCallRequest>> stream(
      serviceStub_.Get()->StreamingInputCall(&context, &response));

  request.mutable_payload()->set_body(grpc::string(27182, '\0'));
  request.mutable_expect_compressed()->set_value(true);
  gpr_log(GPR_DEBUG, "Sending streaming request with compression enabled");
  if (!stream->Write(request)) {
    gpr_log(GPR_ERROR, "%s(): stream->Write() failed", __func__);
    return TransientFailureOrAbort();
  }

  WriteOptions wopts;
  wopts.set_no_compression();
  request.mutable_payload()->set_body(grpc::string(45904, '\0'));
  request.mutable_expect_compressed()->set_value(false);
  gpr_log(GPR_DEBUG, "Sending streaming request with compression disabled");
  if (!stream->Write(request, wopts)) {
    gpr_log(GPR_ERROR, "%s(): stream->Write() failed", __func__);
    return TransientFailureOrAbort();
  }
  GPR_ASSERT(stream->WritesDone());

  s = stream->Finish();
  if (!AssertStatusOk(s)) {
    return false;
  }

  return true;
}

bool InteropClient::DoServerCompressedStreaming() {
  const std::vector<bool> compressions = {true, false};
  const std::vector<int> sizes = {31415, 92653};

  ClientContext context;
  InteropClientContextInspector inspector(context);
  StreamingOutputCallRequest request;

  for (size_t i = 0; i < compressions.size(); i++) {
    for (size_t j = 0; j < sizes.size(); j++) {
      char* log_suffix;
      gpr_asprintf(&log_suffix, "(compression=%s; size=%d)",
                   compressions[i] ? "true" : "false", sizes[j]);

      gpr_log(GPR_DEBUG, "Sending request streaming rpc %s.", log_suffix);
      gpr_free(log_suffix);

      ResponseParameters* const response_parameter =
          request.add_response_parameters();
      response_parameter->mutable_compressed()->set_value(compressions[i]);
      response_parameter->set_size(sizes[j]);
    }
  }
  std::unique_ptr<ClientReader<StreamingOutputCallResponse>> stream(
      serviceStub_.Get()->StreamingOutputCall(&context, request));

  size_t k = 0;
  StreamingOutputCallResponse response;
  while (stream->Read(&response)) {
    // Payload size checks.
    GPR_ASSERT(response.payload().body() ==
               grpc::string(request.response_parameters(k).size(), '\0'));

    // Compression checks.
    GPR_ASSERT(request.response_parameters(k).has_compressed());
    if (request.response_parameters(k).compressed().value()) {
      GPR_ASSERT(inspector.GetCallCompressionAlgorithm() > GRPC_COMPRESS_NONE);
      GPR_ASSERT(inspector.GetMessageFlags() & GRPC_WRITE_INTERNAL_COMPRESS);
    } else {
      // requested *no* compression.
      GPR_ASSERT(!(inspector.GetMessageFlags() & GRPC_WRITE_INTERNAL_COMPRESS));
    }
    ++k;
  }

  if (k < response_stream_sizes.size()) {
    // stream->Read() failed before reading all the expected messages. This
    // is most likely due to a connection failure.
    gpr_log(GPR_ERROR,
            "%s(): Responses read (k=%" PRIuPTR
            ") is "
            "less than the expected messages (i.e "
            "response_stream_sizes.size() (%" PRIuPTR ")).",
            __func__, k, response_stream_sizes.size());
    return TransientFailureOrAbort();
  }

  Status s = stream->Finish();
  if (!AssertStatusOk(s)) {
    return false;
  }
  return true;
}

bool InteropClient::DoResponseStreamingWithSlowConsumer() {
  gpr_log(GPR_DEBUG, "Receiving response streaming rpc with slow consumer ...");

  ClientContext context;
  StreamingOutputCallRequest request;

  for (int i = 0; i < kNumResponseMessages; ++i) {
    ResponseParameters* response_parameter = request.add_response_parameters();
    response_parameter->set_size(kResponseMessageSize);
  }
  StreamingOutputCallResponse response;
  std::unique_ptr<ClientReader<StreamingOutputCallResponse>> stream(
      serviceStub_.Get()->StreamingOutputCall(&context, request));

  int i = 0;
  while (stream->Read(&response)) {
    GPR_ASSERT(response.payload().body() ==
               grpc::string(kResponseMessageSize, '\0'));
    gpr_log(GPR_DEBUG, "received message %d", i);
    usleep(kReceiveDelayMilliSeconds * 1000);
    ++i;
  }

  if (i < kNumResponseMessages) {
    gpr_log(GPR_ERROR,
            "DoResponseStreamingWithSlowConsumer(): Responses read (i=%d) is "
            "less than the expected messages (i.e kNumResponseMessages = %d)",
            i, kNumResponseMessages);

    return TransientFailureOrAbort();
  }

  Status s = stream->Finish();
  if (!AssertStatusOk(s)) {
    return false;
  }

  gpr_log(GPR_DEBUG, "Response streaming done.");
  return true;
}

bool InteropClient::DoHalfDuplex() {
  gpr_log(GPR_DEBUG, "Sending half-duplex streaming rpc ...");

  ClientContext context;
  std::unique_ptr<ClientReaderWriter<StreamingOutputCallRequest,
                                     StreamingOutputCallResponse>>
      stream(serviceStub_.Get()->HalfDuplexCall(&context));

  StreamingOutputCallRequest request;
  ResponseParameters* response_parameter = request.add_response_parameters();
  for (unsigned int i = 0; i < response_stream_sizes.size(); ++i) {
    response_parameter->set_size(response_stream_sizes[i]);

    if (!stream->Write(request)) {
      gpr_log(GPR_ERROR, "DoHalfDuplex(): stream->Write() failed. i=%d", i);
      return TransientFailureOrAbort();
    }
  }
  stream->WritesDone();

  unsigned int i = 0;
  StreamingOutputCallResponse response;
  while (stream->Read(&response)) {
    GPR_ASSERT(response.payload().body() ==
               grpc::string(response_stream_sizes[i], '\0'));
    ++i;
  }

  if (i < response_stream_sizes.size()) {
    // stream->Read() failed before reading all the expected messages. This is
    // most likely due to a connection failure
    gpr_log(GPR_ERROR,
            "DoHalfDuplex(): Responses read (i=%d) are less than the expected "
            "number of messages response_stream_sizes.size() (%" PRIuPTR ")",
            i, response_stream_sizes.size());
    return TransientFailureOrAbort();
  }

  Status s = stream->Finish();
  if (!AssertStatusOk(s)) {
    return false;
  }

  gpr_log(GPR_DEBUG, "Half-duplex streaming rpc done.");
  return true;
}

bool InteropClient::DoPingPong() {
  gpr_log(GPR_DEBUG, "Sending Ping Pong streaming rpc ...");

  ClientContext context;
  std::unique_ptr<ClientReaderWriter<StreamingOutputCallRequest,
                                     StreamingOutputCallResponse>>
      stream(serviceStub_.Get()->FullDuplexCall(&context));

  StreamingOutputCallRequest request;
  ResponseParameters* response_parameter = request.add_response_parameters();
  Payload* payload = request.mutable_payload();
  StreamingOutputCallResponse response;

  for (unsigned int i = 0; i < request_stream_sizes.size(); ++i) {
    response_parameter->set_size(response_stream_sizes[i]);
    payload->set_body(grpc::string(request_stream_sizes[i], '\0'));

    if (!stream->Write(request)) {
      gpr_log(GPR_ERROR, "DoPingPong(): stream->Write() failed. i: %d", i);
      return TransientFailureOrAbort();
    }

    if (!stream->Read(&response)) {
      gpr_log(GPR_ERROR, "DoPingPong(): stream->Read() failed. i:%d", i);
      return TransientFailureOrAbort();
    }

    GPR_ASSERT(response.payload().body() ==
               grpc::string(response_stream_sizes[i], '\0'));
  }

  stream->WritesDone();

  GPR_ASSERT(!stream->Read(&response));

  Status s = stream->Finish();
  if (!AssertStatusOk(s)) {
    return false;
  }

  gpr_log(GPR_DEBUG, "Ping pong streaming done.");
  return true;
}

bool InteropClient::DoCancelAfterBegin() {
  gpr_log(GPR_DEBUG, "Sending request streaming rpc ...");

  ClientContext context;
  StreamingInputCallRequest request;
  StreamingInputCallResponse response;

  std::unique_ptr<ClientWriter<StreamingInputCallRequest>> stream(
      serviceStub_.Get()->StreamingInputCall(&context, &response));

  gpr_log(GPR_DEBUG, "Trying to cancel...");
  context.TryCancel();
  Status s = stream->Finish();

  if (!AssertStatusCode(s, StatusCode::CANCELLED)) {
    return false;
  }

  gpr_log(GPR_DEBUG, "Canceling streaming done.");
  return true;
}

bool InteropClient::DoCancelAfterFirstResponse() {
  gpr_log(GPR_DEBUG, "Sending Ping Pong streaming rpc ...");

  ClientContext context;
  std::unique_ptr<ClientReaderWriter<StreamingOutputCallRequest,
                                     StreamingOutputCallResponse>>
      stream(serviceStub_.Get()->FullDuplexCall(&context));

  StreamingOutputCallRequest request;
  ResponseParameters* response_parameter = request.add_response_parameters();
  response_parameter->set_size(31415);
  request.mutable_payload()->set_body(grpc::string(27182, '\0'));
  StreamingOutputCallResponse response;

  if (!stream->Write(request)) {
    gpr_log(GPR_ERROR, "DoCancelAfterFirstResponse(): stream->Write() failed");
    return TransientFailureOrAbort();
  }

  if (!stream->Read(&response)) {
    gpr_log(GPR_ERROR, "DoCancelAfterFirstResponse(): stream->Read failed");
    return TransientFailureOrAbort();
  }
  GPR_ASSERT(response.payload().body() == grpc::string(31415, '\0'));

  gpr_log(GPR_DEBUG, "Trying to cancel...");
  context.TryCancel();

  Status s = stream->Finish();
  gpr_log(GPR_DEBUG, "Canceling pingpong streaming done.");
  return true;
}

bool InteropClient::DoTimeoutOnSleepingServer() {
  gpr_log(GPR_DEBUG,
          "Sending Ping Pong streaming rpc with a short deadline...");

  ClientContext context;
  std::chrono::system_clock::time_point deadline =
      std::chrono::system_clock::now() + std::chrono::milliseconds(1);
  context.set_deadline(deadline);
  std::unique_ptr<ClientReaderWriter<StreamingOutputCallRequest,
                                     StreamingOutputCallResponse>>
      stream(serviceStub_.Get()->FullDuplexCall(&context));

  StreamingOutputCallRequest request;
  request.mutable_payload()->set_body(grpc::string(27182, '\0'));
  stream->Write(request);

  Status s = stream->Finish();
  if (!AssertStatusCode(s, StatusCode::DEADLINE_EXCEEDED)) {
    return false;
  }

  gpr_log(GPR_DEBUG, "Pingpong streaming timeout done.");
  return true;
}

bool InteropClient::DoEmptyStream() {
  gpr_log(GPR_DEBUG, "Starting empty_stream.");

  ClientContext context;
  std::unique_ptr<ClientReaderWriter<StreamingOutputCallRequest,
                                     StreamingOutputCallResponse>>
      stream(serviceStub_.Get()->FullDuplexCall(&context));
  stream->WritesDone();
  StreamingOutputCallResponse response;
  GPR_ASSERT(stream->Read(&response) == false);

  Status s = stream->Finish();
  if (!AssertStatusOk(s)) {
    return false;
  }

  gpr_log(GPR_DEBUG, "empty_stream done.");
  return true;
}

bool InteropClient::DoStatusWithMessage() {
  gpr_log(GPR_DEBUG,
          "Sending RPC with a request for status code 2 and message");

  ClientContext context;
  SimpleRequest request;
  SimpleResponse response;
  EchoStatus* requested_status = request.mutable_response_status();
  requested_status->set_code(grpc::StatusCode::UNKNOWN);
  grpc::string test_msg = "This is a test message";
  requested_status->set_message(test_msg);

  Status s = serviceStub_.Get()->UnaryCall(&context, request, &response);

  if (!AssertStatusCode(s, grpc::StatusCode::UNKNOWN)) {
    return false;
  }

  GPR_ASSERT(s.error_message() == test_msg);
  gpr_log(GPR_DEBUG, "Done testing Status and Message");
  return true;
}

bool InteropClient::DoCustomMetadata() {
  const grpc::string kEchoInitialMetadataKey("x-grpc-test-echo-initial");
  const grpc::string kInitialMetadataValue("test_initial_metadata_value");
  const grpc::string kEchoTrailingBinMetadataKey(
      "x-grpc-test-echo-trailing-bin");
  const grpc::string kTrailingBinValue("\x0a\x0b\x0a\x0b\x0a\x0b");
  ;

  {
    gpr_log(GPR_DEBUG, "Sending RPC with custom metadata");
    ClientContext context;
    context.AddMetadata(kEchoInitialMetadataKey, kInitialMetadataValue);
    context.AddMetadata(kEchoTrailingBinMetadataKey, kTrailingBinValue);
    SimpleRequest request;
    SimpleResponse response;
    request.set_response_size(kLargeResponseSize);
    grpc::string payload(kLargeRequestSize, '\0');
    request.mutable_payload()->set_body(payload.c_str(), kLargeRequestSize);

    Status s = serviceStub_.Get()->UnaryCall(&context, request, &response);
    if (!AssertStatusOk(s)) {
      return false;
    }

    const auto& server_initial_metadata = context.GetServerInitialMetadata();
    auto iter = server_initial_metadata.find(kEchoInitialMetadataKey);
    GPR_ASSERT(iter != server_initial_metadata.end());
    GPR_ASSERT(iter->second.data() == kInitialMetadataValue);
    const auto& server_trailing_metadata = context.GetServerTrailingMetadata();
    iter = server_trailing_metadata.find(kEchoTrailingBinMetadataKey);
    GPR_ASSERT(iter != server_trailing_metadata.end());
    GPR_ASSERT(grpc::string(iter->second.begin(), iter->second.end()) ==
               kTrailingBinValue);

    gpr_log(GPR_DEBUG, "Done testing RPC with custom metadata");
  }

  {
    gpr_log(GPR_DEBUG, "Sending stream with custom metadata");
    ClientContext context;
    context.AddMetadata(kEchoInitialMetadataKey, kInitialMetadataValue);
    context.AddMetadata(kEchoTrailingBinMetadataKey, kTrailingBinValue);
    std::unique_ptr<ClientReaderWriter<StreamingOutputCallRequest,
                                       StreamingOutputCallResponse>>
        stream(serviceStub_.Get()->FullDuplexCall(&context));

    StreamingOutputCallRequest request;
    ResponseParameters* response_parameter = request.add_response_parameters();
    response_parameter->set_size(kLargeResponseSize);
    grpc::string payload(kLargeRequestSize, '\0');
    request.mutable_payload()->set_body(payload.c_str(), kLargeRequestSize);
    StreamingOutputCallResponse response;

    if (!stream->Write(request)) {
      gpr_log(GPR_ERROR, "DoCustomMetadata(): stream->Write() failed");
      return TransientFailureOrAbort();
    }

    stream->WritesDone();

    if (!stream->Read(&response)) {
      gpr_log(GPR_ERROR, "DoCustomMetadata(): stream->Read() failed");
      return TransientFailureOrAbort();
    }

    GPR_ASSERT(response.payload().body() ==
               grpc::string(kLargeResponseSize, '\0'));

    GPR_ASSERT(!stream->Read(&response));

    Status s = stream->Finish();
    if (!AssertStatusOk(s)) {
      return false;
    }

    const auto& server_initial_metadata = context.GetServerInitialMetadata();
    auto iter = server_initial_metadata.find(kEchoInitialMetadataKey);
    GPR_ASSERT(iter != server_initial_metadata.end());
    GPR_ASSERT(iter->second.data() == kInitialMetadataValue);
    const auto& server_trailing_metadata = context.GetServerTrailingMetadata();
    iter = server_trailing_metadata.find(kEchoTrailingBinMetadataKey);
    GPR_ASSERT(iter != server_trailing_metadata.end());
    GPR_ASSERT(grpc::string(iter->second.begin(), iter->second.end()) ==
               kTrailingBinValue);

    gpr_log(GPR_DEBUG, "Done testing stream with custom metadata");
  }

  return true;
}

}  // namespace testing
}  // namespace grpc
