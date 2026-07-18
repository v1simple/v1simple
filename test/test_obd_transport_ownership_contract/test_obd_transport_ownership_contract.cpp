#include <unity.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    TEST_ASSERT_TRUE_MESSAGE(input.is_open(), path.string().c_str());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::string extractFunction(const std::string& source, const std::string& signature) {
    const size_t signaturePos = source.find(signature);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, signaturePos, signature.c_str());
    const size_t open = source.find('{', signaturePos);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, open, signature.c_str());

    int depth = 0;
    for (size_t pos = open; pos < source.size(); ++pos) {
        if (source[pos] == '{') {
            ++depth;
        } else if (source[pos] == '}') {
            --depth;
            if (depth == 0) {
                return source.substr(open, pos - open + 1);
            }
        }
    }

    TEST_FAIL_MESSAGE("unterminated function body");
    return {};
}

size_t countOccurrences(const std::string& source, const std::string& needle) {
    size_t count = 0;
    size_t offset = 0;
    while ((offset = source.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

std::filesystem::path projectPath(const char* relativePath) {
    return std::filesystem::path(PROJECT_DIR) / relativePath;
}

void assertCallbackPublishesWithoutClearing(const std::string& clientSource, const char* signature) {
    const std::string body = extractFunction(clientSource, signature);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("handleDisconnected(reason)"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("clearCharacteristicHandles"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("serviceDeferredLinkState"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("pTxChar_"));
    TEST_ASSERT_EQUAL(std::string::npos, body.find("pRxChar_"));
}

} // namespace

void setUp() {}
void tearDown() {}

void test_direct_obd_disconnect_calls_are_confined_to_transport_owner() {
    const std::filesystem::path sourceRoot = projectPath("src");
    const std::filesystem::path transportPath = projectPath("src/modules/obd/obd_runtime_transport.cpp");
    const std::string transportSource = readFile(transportPath);
    const std::string dispatchSource = readFile(projectPath("src/modules/obd/obd_transport_control_dispatch.h"));
    const std::string controlBody = extractFunction(transportSource, "bool serviceObdTransportControl");
    const std::string taskBody = extractFunction(transportSource, "void obdTransportTaskEntry");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, controlBody.find("controlDispatch.service("));
    TEST_ASSERT_EQUAL_UINT64(0, countOccurrences(taskBody, "context->bleClient->disconnect("));
    TEST_ASSERT_EQUAL_UINT64(1, countOccurrences(dispatchSource, "client.disconnect()"));
    TEST_ASSERT_EQUAL_UINT64(1, countOccurrences(dispatchSource, "client.deleteBond("));

    for (const auto& entry : std::filesystem::recursive_directory_iterator(sourceRoot)) {
        if (!entry.is_regular_file() || (entry.path().extension() != ".cpp" && entry.path().extension() != ".h")) {
            continue;
        }

        const std::string source = readFile(entry.path());
        TEST_ASSERT_EQUAL_MESSAGE(std::string::npos, source.find("obdBleClient.disconnect("),
                                  entry.path().string().c_str());

        if (entry.path() != transportPath && entry.path().filename() != "obd_transport_control_dispatch.h") {
            TEST_ASSERT_EQUAL_MESSAGE(std::string::npos, source.find("bleClient_->disconnect("),
                                      entry.path().string().c_str());
            TEST_ASSERT_EQUAL_MESSAGE(std::string::npos, source.find("context->bleClient->disconnect("),
                                      entry.path().string().c_str());
        }
    }
}

void test_obd_disconnect_callbacks_publish_link_down_without_clearing_handles() {
    const std::string clientSource = readFile(projectPath("src/modules/obd/obd_ble_client.cpp"));

    assertCallbackPublishesWithoutClearing(clientSource, "void ObdClientCallback::onConnectFail");
    assertCallbackPublishesWithoutClearing(clientSource, "void ObdClientCallback::onDisconnect");

    const std::string disconnectedBody = extractFunction(clientSource, "void ObdBleClient::handleDisconnected");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, disconnectedBody.find("linkDownFence_.publish(generation, reason)"));
    TEST_ASSERT_EQUAL(std::string::npos, disconnectedBody.find("clearCharacteristicHandles"));
    TEST_ASSERT_EQUAL(std::string::npos, disconnectedBody.find("serviceDeferredLinkState"));
    TEST_ASSERT_EQUAL(std::string::npos, disconnectedBody.find("pTxChar_"));
    TEST_ASSERT_EQUAL(std::string::npos, disconnectedBody.find("pRxChar_"));

    const std::string clearStateBody = extractFunction(clientSource, "void ObdBleClient::clearLinkState");
    TEST_ASSERT_EQUAL(std::string::npos, clearStateBody.find("clearCharacteristicHandles"));
    TEST_ASSERT_EQUAL(std::string::npos, clearStateBody.find("pTxChar_"));
    TEST_ASSERT_EQUAL(std::string::npos, clearStateBody.find("pRxChar_"));
}

void test_disconnect_ble_queues_a_coalesced_transport_control_request() {
    const std::string transportSource = readFile(projectPath("src/modules/obd/obd_runtime_transport.cpp"));
    const std::string body = extractFunction(transportSource, "bool ObdRuntimeModule::disconnectBle");
    const std::string queueBody =
        extractFunction(transportSource, "bool ObdRuntimeModule::queuePendingTransportDisconnect");

    TEST_ASSERT_EQUAL(std::string::npos, body.find("bleClient_->disconnect("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("if (transportDisconnectPending_)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("requestEpoch.cancelQueuedWork()"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("transportDisconnectPending_ = true"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("pendingDisconnectRequestId_ = ++nextTransportRequestId_"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, queueBody.find("request.op = ObdTransportOp::DISCONNECT"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, queueBody.find("xQueueOverwrite(sObdTransport.controlQueue"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, queueBody.find("transportDisconnectQueued_ = true"));
}

void test_transport_task_services_link_fence_before_and_after_operations() {
    const std::string transportSource = readFile(projectPath("src/modules/obd/obd_runtime_transport.cpp"));
    const std::string taskBody = extractFunction(transportSource, "void obdTransportTaskEntry");
    const std::string dispatchSource = readFile(projectPath("src/modules/obd/obd_transport_control_dispatch.h"));

    const size_t operationSwitch = taskBody.find("switch (request.op)");
    const size_t serviceBefore = taskBody.rfind("context->bleClient->serviceDeferredLinkState();", operationSwitch);
    const size_t serviceAfter = taskBody.find("context->bleClient->serviceDeferredLinkState();", operationSwitch);
    const size_t publishResult = taskBody.find("publishObdTransportResult(result)", operationSwitch);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, operationSwitch);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, serviceBefore);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, serviceAfter);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, publishResult);
    TEST_ASSERT_TRUE(serviceBefore < operationSwitch);
    TEST_ASSERT_TRUE(operationSwitch < serviceAfter);
    TEST_ASSERT_TRUE(serviceAfter < publishResult);

    const size_t claim = taskBody.find("requestEpoch.tryClaim(request.dispatchEpoch)");
    const size_t release = taskBody.find("requestEpoch.releaseClaim()", operationSwitch);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, claim);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, release);
    TEST_ASSERT_TRUE(claim < operationSwitch);
    TEST_ASSERT_TRUE(operationSwitch < release);

    const size_t disconnect = dispatchSource.find("client.disconnect()");
    const size_t confirmed = dispatchSource.find("client.linkDownConfirmed(targetGeneration_)");
    const size_t acknowledge =
        dispatchSource.find("acknowledge(request_, bondDeleteAttempted_, bondDeleted_, true, false)");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, disconnect);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, confirmed);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, acknowledge);
    TEST_ASSERT_TRUE(disconnect < confirmed);
    TEST_ASSERT_TRUE(confirmed < acknowledge);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_direct_obd_disconnect_calls_are_confined_to_transport_owner);
    RUN_TEST(test_obd_disconnect_callbacks_publish_link_down_without_clearing_handles);
    RUN_TEST(test_disconnect_ble_queues_a_coalesced_transport_control_request);
    RUN_TEST(test_transport_task_services_link_fence_before_and_after_operations);
    return UNITY_END();
}
