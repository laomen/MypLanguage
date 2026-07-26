#ifndef MYLANG_LSP_H
#define MYLANG_LSP_H

#include <string>
#include <functional>

namespace mylang {

/// LSP message: header + JSON body
struct LSPMessage {
    std::string content_type;
    int content_length = 0;
    std::string body;
};

/// Read one LSP message from stdin (blocking).
bool readMessage(LSPMessage& msg);

/// Write an LSP response/notification to stdout.
void sendResponse(const std::string& id, const std::string& body);
void sendNotification(const std::string& method, const std::string& params);

/// Simple JSON builder helpers.
std::string jsonString(const std::string& s);
std::string jsonPair(const std::string& key, const std::string& val);
std::string jsonInt(const std::string& key, int val);
std::string jsonArray(const std::string& items);
std::string jsonObject(const std::string& members);

/// Run the LSP server (never returns).
int runLSPServer(int argc, char** argv);

} // namespace mylang

#endif
