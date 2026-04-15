class PawnRESTCore
{
private:

    // Receive (Server)
    std::unordered_map<int, UploadRoute> routes;
    mutable std::mutex routesMutex;
    std::atomic<int> nextRouteId  { 0 };
    std::atomic<int> nextUploadId { 0 };

    std::unique_ptr<httplib::Server> httpServer;
    std::thread                      httpThread;
    std::thread                      cleanupThread;
    std::atomic<bool>                shutdownFlag { false };
    std::atomic<bool>                isRunning { false };
    std::string                      serverRootPath;
    int                              currentPort = 0;

    std::mutex               eventMutex;
    std::vector<UploadEvent> pendingEvents;

    // Upload (Client)
    std::unordered_map<int, OutgoingUpload> outgoingUploads;
    mutable std::mutex outgoingMutex;
    std::atomic<int> nextOutgoingId { 0 };
    std::unordered_map<int, UploadClient> uploadClients;
    std::atomic<int> nextUploadClientId { 0 };

    std::mutex                       outgoingEventMutex;
    std::vector<OutgoingUploadEvent> pendingOutgoingEvents;

    std::thread uploadWorkerThread;
    std::atomic<bool> uploadWorkerRunning { false };

    // Request (Client)
    std::unordered_map<int, OutgoingRequest> outgoingRequests;
    mutable std::mutex requestMutex;
    std::atomic<int> nextOutgoingRequestId { 0 };
    std::mutex requestEventMutex;
    std::vector<OutgoingRequestEvent> pendingRequestEvents;
    std::thread requestWorkerThread;
    std::atomic<bool> requestWorkerRunning { false };

    // WebSocket (Client)
    std::unordered_map<int, std::shared_ptr<WebSocketConnection>> webSocketConnections;
    mutable std::mutex webSocketMutex;
    std::atomic<int> nextWebSocketId { 0 };
    std::mutex webSocketEventMutex;
    std::vector<WebSocketEvent> pendingWebSocketEvents;

    bool                             tlsEnabled = false;
    std::string                      tlsCertPath;
    std::string                      tlsKeyPath;

    // REST API
    std::unordered_map<int, APIRoute> apiRoutes;
    mutable std::mutex apiRoutesMutex;
    std::atomic<int> nextApiRouteId { 0 };
    std::atomic<int> nextRequestId { 0 };
    
    std::unordered_map<int, std::shared_ptr<RequestContext>> activeRequests;
    mutable std::mutex requestsMutex;

    // JSON Node store
    std::unordered_map<int, Json::NodePtr> jsonNodes;
    mutable std::mutex jsonNodesMutex;
    std::atomic<int> nextJsonNodeId { 1 };
    
    std::mutex apiEventMutex;
    std::vector<APIRequestEvent> pendingApiEvents;
    void (*logSink)(const char*) = nullptr;

private:
    template <typename... Args>
    void PrintLn(const char* fmt, Args... args)
    {
        if (!logSink || !fmt) return;
        char buffer[1024];
        int written = 0;
        if constexpr (sizeof...(Args) == 0) {
            written = std::snprintf(buffer, sizeof(buffer), "%s", fmt);
        } else {
            written = std::snprintf(buffer, sizeof(buffer), fmt, args...);
        }
        if (written < 0) return;
        buffer[sizeof(buffer) - 1] = '\0';
        logSink(buffer);
        if (written >= static_cast<int>(sizeof(buffer))) {
            logSink("[PawnREST] WARNING: log message truncated");
        }
    }

    std::string MakeTempPath(int uploadId)
    {
        auto ts = std::chrono::system_clock::now().time_since_epoch().count();
        return serverRootPath + Config::TEMP_DIR + "upload_" + std::to_string(uploadId) + "_" + std::to_string(ts) + ".tmp";
    }

    std::string ToRelativePath(const std::string& absPath)
    {
        if (absPath.rfind(serverRootPath, 0) == 0)
            return absPath.substr(serverRootPath.size());
        return absPath;
    }

    std::string ResolveDestPath(
        const std::string& dir,
        const std::string& filename,
        ConflictMode mode)
    {
        std::string dest = dir;
        if (!dest.empty() && dest.back() != '/')
            dest += '/';
        dest += filename;

        if (!FileUtils::FileExists(dest))
            return dest;

        switch (mode) {
            case ConflictMode::Overwrite:
                return dest;

            case ConflictMode::Reject:
                return "";

            case ConflictMode::Rename:
            default: {
                size_t dotPos = filename.find_last_of('.');
                std::string stem = (dotPos == std::string::npos) ? filename : filename.substr(0, dotPos);
                std::string ext  = (dotPos == std::string::npos) ? "" : filename.substr(dotPos);

                for (int i = 1; i <= 9999; ++i) {
                    std::string candidate = dir;
                    if (!candidate.empty() && candidate.back() != '/')
                        candidate += '/';
                    candidate += stem + "_" + std::to_string(i) + ext;

                    if (!FileUtils::FileExists(candidate))
                        return candidate;
                }

                auto ts = std::chrono::system_clock::now().time_since_epoch().count();
                std::string fallback = dir;
                if (!fallback.empty() && fallback.back() != '/')
                    fallback += '/';
                fallback += stem + "_" + std::to_string(ts) + ext;
                return fallback;
            }
        }
    }

    ValidationResult ValidateUpload(
        const std::string& filename,
        size_t fileSize,
        const UploadRoute& route)
    {
        if (!route.allowedExtensions.empty()) {
            size_t dotPos = filename.find_last_of('.');
            std::string ext = (dotPos == std::string::npos) ? "" : filename.substr(dotPos);

            std::transform(ext.begin(), ext.end(), ext.begin(),
                [](unsigned char c){ return std::tolower(c); });

            bool allowed = std::any_of(
                route.allowedExtensions.begin(),
                route.allowedExtensions.end(),
                [&](const std::string& e) {
                    std::string eLower = e;
                    std::transform(eLower.begin(), eLower.end(), eLower.begin(),
                        [](unsigned char c){ return std::tolower(c); });
                    return eLower == ext;
                }
            );
            if (!allowed)
                return { false, "extension not allowed: " + ext };
        }

        if (fileSize > route.maxSizeBytes)
            return { false, "file too large (" + std::to_string(fileSize) + " bytes)" };

        return { true, "" };
    }

    void PushEvent(UploadEvent ev)
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        pendingEvents.push_back(std::move(ev));
    }

    void PushOutgoingEvent(OutgoingUploadEvent ev)
    {
        std::lock_guard<std::mutex> lock(outgoingEventMutex);
        pendingOutgoingEvents.push_back(std::move(ev));
    }

    void PushRequestEvent(OutgoingRequestEvent ev)
    {
        std::lock_guard<std::mutex> lock(requestEventMutex);
        pendingRequestEvents.push_back(std::move(ev));
    }

    void PushWebSocketEvent(WebSocketEvent ev)
    {
        std::lock_guard<std::mutex> lock(webSocketEventMutex);
        pendingWebSocketEvents.push_back(std::move(ev));
    }

    void EmitWebSocketDisconnect(
        const std::shared_ptr<WebSocketConnection>& conn,
        int status,
        const std::string& reason,
        int errorCode)
    {
        if (!conn) return;

        bool expected = false;
        if (!conn->disconnectEmitted.compare_exchange_strong(expected, true)) return;

        PushWebSocketEvent({
            WebSocketEvent::Type::Disconnected,
            conn->socketId,
            conn->callbackName,
            conn->jsonMode,
            "",
            -1,
            status,
            reason,
            errorCode
        });
    }

    bool IsCancelledRequest(int requestId)
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        auto it = outgoingRequests.find(requestId);
        if (it == outgoingRequests.end()) return true;
        return it->second.status == RequestStatus::Cancelled || it->second.cancelToken->load();
    }

    bool IsCancelled(int uploadId)
    {
        std::lock_guard<std::mutex> lock(outgoingMutex);
        auto it = outgoingUploads.find(uploadId);
        if (it == outgoingUploads.end()) return true;
        return it->second.status == UploadStatus::Cancelled || it->second.cancelToken->load();
    }

    void UpdateOutgoingProgress(int uploadId, size_t current, size_t total, uint32_t crcValue)
    {
        int pct = 0;
        if (total > 0) pct = static_cast<int>((current * 100) / total);

        {
            std::lock_guard<std::mutex> lock(outgoingMutex);
            auto it = outgoingUploads.find(uploadId);
            if (it != outgoingUploads.end()) {
                it->second.progressPct = pct;
                it->second.bytesUploaded = current;
                it->second.totalBytes = total;
            }
        }

        PushOutgoingEvent({
            OutgoingUploadEvent::Type::Progress,
            uploadId, pct, crcValue, 0, OutgoingError::NONE, "", "", ""
        });
    }

    void HandleUploadRaw(
        const httplib::Request& req,
        httplib::Response& res,
        const httplib::ContentReader& content_reader,
        int routeId,
        const UploadRoute& route,
        const std::string& expectedCrcHex,
        uint32_t expectedCrc)
    {
        int uploadId = nextUploadId++;
        CRC32 crc32;

        std::string final_filename = SanitizeFilename(req.get_header_value("X-Filename"));
        if (final_filename.empty()) {
            res.status = 400;
            res.set_content(Json::Obj({
                Json::Str("error", "missing or invalid X-Filename", false)
            }), "application/json");
            return;
        }

        size_t expected_size = 0;
        auto clen = req.get_header_value("Content-Length");
        if (!clen.empty()) {
            try { expected_size = static_cast<size_t>(std::stoull(clen)); } catch(...) {}
        }

        auto validation = ValidateUpload(final_filename, expected_size, route);
        if (!validation.ok && expected_size > 0) {
            res.status = 422;
            res.set_content(Json::Obj({
                Json::Str("error", validation.reason, false)
            }), "application/json");
            return;
        }

        FileUtils::CreateDirectory(serverRootPath + Config::TEMP_DIR);
        std::string tempPath = MakeTempPath(uploadId);
        std::ofstream tempFile(tempPath, std::ios::binary);
        if (!tempFile.is_open()) {
            res.status = 500;
            res.set_content(Json::Obj({
                Json::Str("error", "io error: cannot open temp file", false)
            }), "application/json");
            return;
        }

        size_t written = 0;
        int lastPctBucket = -1;
        bool has_error = false;
        std::string errstr;

        content_reader([&](const char* data, size_t data_length) {
            if (has_error) return false;

            if (written + data_length > route.maxSizeBytes) {
                has_error = true;
                errstr = "file too large (" + std::to_string(written + data_length) + " bytes)";
                return false;
            }

            crc32.update(data, data_length);

            tempFile.write(data, data_length);
            if (tempFile.fail()) {
                has_error = true;
                errstr = "write error";
                return false;
            }

            written += data_length;

            if (expected_size > 0) {
                int pct = static_cast<int>((written * 100) / expected_size);
                int bucket = pct / 10;
                if (bucket != lastPctBucket) {
                    lastPctBucket = bucket;
                    PushEvent({
                        UploadEvent::Type::Progress, uploadId, routeId,
                        route.endpoint, final_filename, "", "", pct
                    });
                }
            }

            return true;
        });

        tempFile.close();

        uint32_t finalCrc = crc32.final();
        std::string finalCrcHex = CRC32::toHex(finalCrc);

        if (has_error) {
            FileUtils::RemoveFile(tempPath);
            res.status = (errstr.find("write") != std::string::npos) ? 500 : 422;
            res.set_content(Json::Obj({
                Json::Str("error", errstr, false)
            }), "application/json");

            PushEvent({
                UploadEvent::Type::Failed, uploadId, routeId, route.endpoint,
                final_filename, "", errstr, 0, finalCrc, false, expectedCrcHex
            });
            return;
        }

        validation = ValidateUpload(final_filename, written, route);
        if (!validation.ok) {
            FileUtils::RemoveFile(tempPath);
            res.status = 422;
            res.set_content(Json::Obj({
                Json::Str("error", validation.reason, false)
            }), "application/json");

            PushEvent({
                UploadEvent::Type::Failed, uploadId, routeId, route.endpoint,
                final_filename, "", validation.reason, 0, finalCrc, false, expectedCrcHex
            });
            return;
        }

        bool crcMatched = true;
        if (expectedCrc != 0 && finalCrc != expectedCrc) {
            crcMatched = false;

            switch (route.onCorrupt) {
                case CorruptAction::Quarantine: {
                    FileUtils::CreateDirectory(serverRootPath + route.quarantinePath);
                    std::string qPath = serverRootPath + route.quarantinePath +
                        final_filename + "." + std::to_string(uploadId) + ".corrupt";
                    FileUtils::RenameFile(tempPath, qPath);
                    break;
                }
                case CorruptAction::Keep: {
                    std::string cPath = serverRootPath + route.destinationPath +
                        final_filename + ".corrupt";
                    FileUtils::RenameFile(tempPath, cPath);
                    break;
                }
                case CorruptAction::Delete:
                default:
                    FileUtils::RemoveFile(tempPath);
                    break;
            }

            res.status = 422;
            res.set_content(Json::Obj({
                Json::Str("error", "crc32 mismatch"),
                Json::Str("received", finalCrcHex),
                Json::Str("expected", expectedCrcHex, false)
            }), "application/json");

            PushEvent({
                UploadEvent::Type::Failed, uploadId, routeId, route.endpoint,
                final_filename, "", "CRC32 mismatch: got " + finalCrcHex + ", expected " + expectedCrcHex,
                100, finalCrc, false, expectedCrcHex
            });
            return;
        }

        std::string finalPath = ResolveDestPath(
            serverRootPath + route.destinationPath,
            final_filename,
            route.onConflict
        );

        if (finalPath.empty()) {
            FileUtils::RemoveFile(tempPath);
            res.status = 409;
            res.set_content(Json::Obj({
                Json::Str("error", "file already exists", false)
            }), "application/json");

            PushEvent({
                UploadEvent::Type::Failed, uploadId, routeId, route.endpoint,
                final_filename, "", "conflict: file already exists", 0, finalCrc, true, expectedCrcHex
            });
            return;
        }

        if (!FileUtils::RenameFile(tempPath, finalPath)) {
            if (!FileUtils::CopyFile(tempPath, finalPath)) {
                FileUtils::RemoveFile(tempPath);
                res.status = 500;
                res.set_content(Json::Obj({
                    Json::Str("error", "failed to move file", false)
                }), "application/json");

                PushEvent({
                    UploadEvent::Type::Failed, uploadId, routeId, route.endpoint,
                    final_filename, "", "move error", 0, finalCrc, true, expectedCrcHex
                });
                return;
            }
            FileUtils::RemoveFile(tempPath);
        }

        std::string relativePath = ToRelativePath(finalPath);

        PushEvent({
            UploadEvent::Type::Completed,
            uploadId, routeId,
            route.endpoint,
            final_filename,
            relativePath,
            "", 100, finalCrc, crcMatched, expectedCrcHex
        });

        res.status = 200;
        res.set_content(Json::Obj({
            Json::Num("uploadId", uploadId),
            Json::Str("path", relativePath),
            Json::Str("crc32", finalCrcHex),
            Json::Num("size", static_cast<long long>(written), false)
        }), "application/json");
    }

    void HandleUploadMultipart(
        const httplib::Request& req,
        httplib::Response& res,
        const httplib::ContentReader& content_reader,
        int routeId,
        const UploadRoute& route,
        const std::string& expectedCrcHex,
        uint32_t expectedCrc)
    {
        int uploadId = nextUploadId++;
        CRC32 crc32;

        std::string final_filename;
        std::string tempPath;
        std::ofstream tempFile;
        bool is_target_file = false;
        size_t written = 0;
        int lastPctBucket = -1;
        bool has_error = false;
        std::string errstr;

        size_t expected_size = 0;
        auto clen = req.get_header_value("Content-Length");
        if (!clen.empty()) {
            try { expected_size = static_cast<size_t>(std::stoull(clen)); } catch(...) {}
        }

        content_reader(
            [&](const httplib::FormData& file) {
                if (file.name == "file") {
                    is_target_file = true;
                    final_filename = SanitizeFilename(file.filename);
                    crc32.reset();

                    if (final_filename.empty()) {
                        is_target_file = false;
                        has_error = true;
                        errstr = "invalid filename";
                        return false;
                    }

                    auto validation = ValidateUpload(final_filename, expected_size, route);
                    if (!validation.ok && expected_size > 0) {
                        is_target_file = false;
                        has_error = true;
                        errstr = validation.reason;
                        return false;
                    }

                    FileUtils::CreateDirectory(serverRootPath + Config::TEMP_DIR);
                    tempPath = MakeTempPath(uploadId);

                    tempFile.open(tempPath, std::ios::binary);
                    if (!tempFile.is_open()) {
                        has_error = true;
                        errstr = "io error: cannot open temp file";
                        return false;
                    }
                } else {
                    is_target_file = false;
                }
                return true;
            },
            [&](const char* data, size_t data_length) {
                if (has_error) return false;
                if (!is_target_file) return true;

                if (written + data_length > route.maxSizeBytes) {
                    has_error = true;
                    errstr = "file too large (" + std::to_string(written + data_length) + " bytes)";
                    return false;
                }

                crc32.update(data, data_length);

                tempFile.write(data, data_length);
                if (tempFile.fail()) {
                    has_error = true;
                    errstr = "write error";
                    return false;
                }

                written += data_length;
                if (expected_size > 0) {
                    int pct = static_cast<int>((written * 100) / expected_size);
                    int bucket = pct / 10;
                    if (bucket != lastPctBucket) {
                        lastPctBucket = bucket;
                        PushEvent({
                            UploadEvent::Type::Progress, uploadId, routeId,
                            route.endpoint, final_filename, "", "", pct
                        });
                    }
                }
                return true;
            }
        );

        if (tempFile.is_open()) tempFile.close();

        uint32_t finalCrc = crc32.final();
        std::string finalCrcHex = CRC32::toHex(finalCrc);

        if (has_error) {
            res.status = 400;
            if (errstr.find("io error") != std::string::npos || errstr.find("write error") != std::string::npos) {
                res.status = 500;
            } else if (errstr.find("too large") != std::string::npos || errstr.find("extension") != std::string::npos) {
                res.status = 422;
            }
            res.set_content(Json::Obj({
                Json::Str("error", errstr, false)
            }), "application/json");

            if (!tempPath.empty()) {
                FileUtils::RemoveFile(tempPath);
            }
            PushEvent({
                UploadEvent::Type::Failed, uploadId, routeId, route.endpoint,
                final_filename, "", errstr, 0, finalCrc, false, expectedCrcHex
            });
            return;
        }

        if (final_filename.empty()) {
            res.status = 400;
            res.set_content(Json::Obj({
                Json::Str("error", "no file field found", false)
            }), "application/json");
            return;
        }

        auto validation = ValidateUpload(final_filename, written, route);
        if (!validation.ok) {
            res.status = 422;
            res.set_content(Json::Obj({
                Json::Str("error", validation.reason, false)
            }), "application/json");
            FileUtils::RemoveFile(tempPath);
            PushEvent({
                UploadEvent::Type::Failed, uploadId, routeId, route.endpoint,
                final_filename, "", validation.reason, 0, finalCrc, false, expectedCrcHex
            });
            return;
        }

        bool crcMatched = true;
        if (expectedCrc != 0 && finalCrc != expectedCrc) {
            crcMatched = false;

            switch (route.onCorrupt) {
                case CorruptAction::Quarantine: {
                    FileUtils::CreateDirectory(serverRootPath + route.quarantinePath);
                    std::string qPath = serverRootPath + route.quarantinePath +
                        final_filename + "." + std::to_string(uploadId) + ".corrupt";
                    FileUtils::RenameFile(tempPath, qPath);
                    break;
                }
                case CorruptAction::Keep: {
                    std::string cPath = serverRootPath + route.destinationPath +
                        final_filename + ".corrupt";
                    FileUtils::RenameFile(tempPath, cPath);
                    break;
                }
                case CorruptAction::Delete:
                default:
                    FileUtils::RemoveFile(tempPath);
                    break;
            }

            res.status = 422;
            res.set_content(Json::Obj({
                Json::Str("error", "crc32 mismatch"),
                Json::Str("received", finalCrcHex),
                Json::Str("expected", expectedCrcHex, false)
            }), "application/json");

            PushEvent({
                UploadEvent::Type::Failed, uploadId, routeId, route.endpoint,
                final_filename, "", "CRC32 mismatch: got " + finalCrcHex + ", expected " + expectedCrcHex,
                100, finalCrc, false, expectedCrcHex
            });
            return;
        }

        std::string finalPath = ResolveDestPath(
            serverRootPath + route.destinationPath,
            final_filename,
            route.onConflict
        );

        if (finalPath.empty()) {
            FileUtils::RemoveFile(tempPath);
            res.status = 409;
            res.set_content(Json::Obj({
                Json::Str("error", "file already exists", false)
            }), "application/json");
            PushEvent({
                UploadEvent::Type::Failed, uploadId, routeId, route.endpoint,
                final_filename, "", "conflict: file already exists", 0, finalCrc, true, expectedCrcHex
            });
            return;
        }

        if (!FileUtils::RenameFile(tempPath, finalPath)) {
            if (!FileUtils::CopyFile(tempPath, finalPath)) {
                FileUtils::RemoveFile(tempPath);
                res.status = 500;
                res.set_content(Json::Obj({
                    Json::Str("error", "failed to move file", false)
                }), "application/json");
                PushEvent({
                    UploadEvent::Type::Failed, uploadId, routeId, route.endpoint,
                    final_filename, "", "move error", 0, finalCrc, true, expectedCrcHex
                });
                return;
            }
            FileUtils::RemoveFile(tempPath);
        }

        std::string relativePath = ToRelativePath(finalPath);

        PushEvent({
            UploadEvent::Type::Completed,
            uploadId, routeId,
            route.endpoint,
            final_filename,
            relativePath,
            "", 100, finalCrc, crcMatched, expectedCrcHex
        });

        res.status = 200;
        res.set_content(Json::Obj({
            Json::Num("uploadId", uploadId),
            Json::Str("path", relativePath),
            Json::Str("crc32", finalCrcHex),
            Json::Num("size", static_cast<long long>(written), false)
        }), "application/json");
    }

    // ─────────────────────────────────────────────────────────────────────────
    // RECEIVE ENTRY
    // ─────────────────────────────────────────────────────────────────────────
    void HandleUploadStreaming(
        const httplib::Request& req,
        httplib::Response& res,
        const httplib::ContentReader& content_reader,
        int routeId)
    {
        std::unique_lock<std::mutex> lock(routesMutex);
        auto it = routes.find(routeId);
        if (it == routes.end()) {
            res.status = 404;
            res.set_content(Json::Obj({
                Json::Str("error", "route not found", false)
            }), "application/json");
            return;
        }
        const UploadRoute route = it->second;
        lock.unlock();

        // Auth
        std::string authKey;
        auto authHeader = req.get_header_value("Authorization");
        if (authHeader.size() > 7 && authHeader.substr(0, 7) == "Bearer ")
            authKey = authHeader.substr(7);

        if (!route.authorizedKeys.empty() &&
            route.authorizedKeys.find(authKey) == route.authorizedKeys.end())
        {
            res.status = 401;
            res.set_content(Json::Obj({
                Json::Str("error", "unauthorized", false)
            }), "application/json");
            return;
        }

        // CRC32
        std::string expectedCrcHex = req.get_header_value("X-File-CRC32");
        uint32_t expectedCrc = expectedCrcHex.empty() ? 0 : CRC32::fromHex(expectedCrcHex);

        if (route.requireCrc32 && expectedCrcHex.empty()) {
            res.status = 400;
            res.set_content(Json::Obj({
                Json::Str("error", "X-File-CRC32 header required", false)
            }), "application/json");
            return;
        }

        if (req.is_multipart_form_data()) {
            HandleUploadMultipart(req, res, content_reader, routeId, route, expectedCrcHex, expectedCrc);
        } else {
            HandleUploadRaw(req, res, content_reader, routeId, route, expectedCrcHex, expectedCrc);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // UPLOAD (CLIENT)
    // ═══════════════════════════════════════════════════════════════════════
    void UploadWorker()
    {
        while (uploadWorkerRunning) {
            int uploadId = -1;
            {
                std::lock_guard<std::mutex> lock(outgoingMutex);
                for (auto& [id, upload] : outgoingUploads) {
                    if (upload.status == UploadStatus::Pending) {
                        upload.status = UploadStatus::Uploading;
                        uploadId = id;
                        break;
                    }
                }
            }

            if (uploadId == -1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            ProcessOutgoingUpload(uploadId);
        }
    }

    template <typename ClientT>
    void ConfigureHttpClient(ClientT& client)
    {
        client.set_connection_timeout(Config::UPLOAD_TIMEOUT_SEC);
        client.set_read_timeout(Config::UPLOAD_TIMEOUT_SEC);
        client.set_write_timeout(Config::UPLOAD_TIMEOUT_SEC);
        client.set_follow_location(true);
    }

    void ProcessOutgoingUpload(int uploadId)
    {
        OutgoingUpload upload;
        {
            std::lock_guard<std::mutex> lock(outgoingMutex);
            auto it = outgoingUploads.find(uploadId);
            if (it == outgoingUploads.end()) return;
            upload = it->second;
        }

        PushOutgoingEvent({
            OutgoingUploadEvent::Type::Started,
            uploadId, 0, 0, 0, 0, "", "", ""
        });

        std::string scheme, host, path;
        int port = 80;
        if (!ParseUrl(upload.url, scheme, host, port, path)) {
            FailOutgoingUpload(uploadId, OutgoingError::INVALID_URL, "invalid_url", "invalid URL");
            return;
        }

        if (scheme != "http" && scheme != "https") {
            FailOutgoingUpload(uploadId, OutgoingError::UNSUPPORTED_SCHEME, "unsupported_scheme", "only http:// and https:// are supported");
            return;
        }

        std::string fullPath = serverRootPath + upload.filepath;
        if (!FileUtils::FileExists(fullPath)) {
            FailOutgoingUpload(uploadId, OutgoingError::FILE_NOT_FOUND, "file_not_found", "file not found: " + fullPath);
            return;
        }

        size_t fileSize = FileUtils::FileSize(fullPath);
        if (fileSize == 0) {
            FailOutgoingUpload(uploadId, OutgoingError::EMPTY_FILE, "empty_file", "empty file");
            return;
        }

        uint32_t crcValue = 0;
        std::string crcHex;
        if (upload.calculateCrc32) {
            crcValue = CRC32::fileChecksum(fullPath);
            crcHex = CRC32::toHex(crcValue);
        }

        {
            std::lock_guard<std::mutex> lock(outgoingMutex);
            auto it = outgoingUploads.find(uploadId);
            if (it != outgoingUploads.end()) {
                it->second.totalBytes = fileSize;
            }
        }

        httplib::Headers headers;
        auto setHeader = [&headers](const std::string& key, const std::string& value) {
            SetOrReplaceHeader(headers, key, value);
        };

        if (!upload.authKey.empty()) {
            setHeader("Authorization", "Bearer " + upload.authKey);
        }
        if (!crcHex.empty()) {
            setHeader("X-File-CRC32", crcHex);
        }
        if (!upload.customHeader.empty()) {
            auto pairs = SplitHeaderPairs(upload.customHeader);
            for (const auto& [key, val] : pairs) {
                setHeader(key, val);
            }
        }

        auto cancelToken = upload.cancelToken;

        auto runUpload = [&](auto& client) -> httplib::Result {
            if (upload.mode == UploadMode::Raw) {
                return UploadRaw(client, uploadId, fullPath, upload.filename, fileSize, headers, crcValue, path, cancelToken);
            }
            return UploadMultipart(client, uploadId, fullPath, upload.filename, fileSize, headers, crcValue, path, cancelToken);
        };

        httplib::Result res(nullptr, httplib::Error::Unknown);
        if (scheme == "https") {
#if PAWNREST_HAS_SSL
            httplib::SSLClient client(host, port);
            if (!client.is_valid()) {
                FailOutgoingUpload(uploadId, OutgoingError::TLS_INVALID_CERTS, "tls", "failed to initialize TLS client");
                return;
            }
            ConfigureHttpClient(client);
            client.enable_server_certificate_verification(upload.verifyTls);
            res = runUpload(client);
#else
            FailOutgoingUpload(uploadId, OutgoingError::TLS_UNAVAILABLE, "tls_unavailable", "TLS support is not available in this build");
            return;
#endif
        } else {
            httplib::Client client(host, port);
            ConfigureHttpClient(client);
            res = runUpload(client);
        }

        if (IsCancelled(uploadId)) {
            {
                std::lock_guard<std::mutex> lock(outgoingMutex);
                auto it = outgoingUploads.find(uploadId);
                if (it != outgoingUploads.end()) {
                    it->second.status = UploadStatus::Cancelled;
                    it->second.errorCode = OutgoingError::CANCELLED;
                    it->second.errorType = "cancelled";
                    it->second.errorMessage = "upload cancelled";
                }
            }

            PushOutgoingEvent({
                OutgoingUploadEvent::Type::Failed,
                uploadId, 0, crcValue, 0, OutgoingError::CANCELLED, "cancelled", "", "upload cancelled"
            });
            return;
        }

        if (!res) {
            auto [errorCode, errorType] = ClassifyClientError(res.error());
            FailOutgoingUpload(uploadId, errorCode, errorType, "HTTP request failed: " + httplib::to_string(res.error()));
            return;
        }

        int status = res->status;
        std::string statusErrorMessage = "HTTP status " + std::to_string(status);
        bool isSuccess = status >= 200 && status < 300;

        {
            std::lock_guard<std::mutex> lock(outgoingMutex);
            auto it = outgoingUploads.find(uploadId);
            if (it != outgoingUploads.end()) {
                it->second.httpStatus = status;
                it->second.responseBody = res->body;
                it->second.crc32Checksum = crcValue;
                it->second.bytesUploaded = fileSize;
                it->second.progressPct = 100;

                if (isSuccess) {
                    it->second.status = UploadStatus::Completed;
                    it->second.errorCode = OutgoingError::NONE;
                    it->second.errorType.clear();
                    it->second.errorMessage.clear();
                } else {
                    it->second.status = UploadStatus::Failed;
                    it->second.errorCode = OutgoingError::HTTP_STATUS;
                    it->second.errorType = "http_status";
                    it->second.errorMessage = statusErrorMessage;
                }
            }
        }

        if (isSuccess) {
            PushOutgoingEvent({
                OutgoingUploadEvent::Type::Completed,
                uploadId, 100, crcValue, status, OutgoingError::NONE, "", res->body, ""
            });
        } else {
            PushOutgoingEvent({
                OutgoingUploadEvent::Type::Failed,
                uploadId, 100, crcValue, status, OutgoingError::HTTP_STATUS, "http_status", res->body, statusErrorMessage
            });
        }
    }

    template <typename ClientT>
    httplib::Result UploadRaw(
        ClientT& client,
        int uploadId,
        const std::string& fullPath,
        const std::string& sendFilename,
        size_t fileSize,
        httplib::Headers headers,
        uint32_t crcValue,
        const std::string& path,
        std::shared_ptr<std::atomic<bool>> cancelToken)
    {
        SetOrReplaceHeader(headers, "Content-Type", "application/octet-stream");
        SetOrReplaceHeader(headers, "X-Filename", sendFilename.empty() ? SanitizeFilename(fullPath) : SanitizeFilename(sendFilename));

        auto file = std::make_shared<std::ifstream>(fullPath, std::ios::binary);
        if (!file->is_open()) {
            return httplib::Result(nullptr, httplib::Error::Unknown);
        }

        size_t sent = 0;
        int lastBucket = -1;

        // FIX: use [=] instead of [=, this] for C++17 compatibility
        auto provider = [=](size_t offset, size_t length, httplib::DataSink& sink) mutable {
            (void)offset;

            if (cancelToken->load()) {
                sink.done();
                return false;
            }

            std::vector<char> buf(Config::UPLOAD_CHUNK_SIZE);
            file->read(buf.data(), static_cast<std::streamsize>(std::min(length, buf.size())));
            std::streamsize got = file->gcount();

            if (got > 0) {
                sink.write(buf.data(), static_cast<size_t>(got));
                sent += static_cast<size_t>(got);
                return true;
            }

            sink.done();
            return true;
        };

        // Progress updates happen via a separate polling mechanism or
        // we track bytes in the provider and call UpdateOutgoingProgress.
        // Since provider captures by value and UpdateOutgoingProgress needs 'this',
        // we do a simpler approach: post-upload final progress.
        // For real progress during raw upload, use a wrapper approach below.

        // Use ContentProviderWithoutLength for streaming
        httplib::Result result = client.Post(path, headers, fileSize, provider, "application/octet-stream");

        file->close();

        if (result && !cancelToken->load()) {
            UpdateOutgoingProgress(uploadId, fileSize, fileSize, crcValue);
        }

        return result;
    }

    template <typename ClientT>
    httplib::Result UploadMultipart(
        ClientT& client,
        int uploadId,
        const std::string& fullPath,
        const std::string& sendFilename,
        size_t fileSize,
        httplib::Headers headers,
        uint32_t crcValue,
        const std::string& path,
        std::shared_ptr<std::atomic<bool>> cancelToken)
    {
        // NOTE:
        // httplib multipart streaming API varies between versions.
        // For large files, prefer RAW mode which is fully streaming.
        // Multipart here uses a memory buffer for compatibility stability.

        std::ifstream file(fullPath, std::ios::binary);
        if (!file) {
            return httplib::Result(nullptr, httplib::Error::Unknown);
        }

        std::vector<char> buffer(fileSize);
        file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
        if (!file) {
            return httplib::Result(nullptr, httplib::Error::Unknown);
        }

        UpdateOutgoingProgress(uploadId, fileSize / 2, fileSize, crcValue);

        // FIX: use UploadFormDataItems and UploadFormData with correct field names
        httplib::UploadFormDataItems items;
        httplib::UploadFormData fileItem;
        fileItem.name         = "file";
        fileItem.filename     = sendFilename.empty() ? SanitizeFilename(fullPath) : SanitizeFilename(sendFilename);
        fileItem.content_type = "application/octet-stream";
        fileItem.content.assign(buffer.data(), buffer.size());
        items.push_back(std::move(fileItem));

        if (cancelToken->load()) {
            return httplib::Result(nullptr, httplib::Error::Canceled);
        }

        httplib::Result result = client.Post(path, headers, items);

        if (!cancelToken->load()) {
            UpdateOutgoingProgress(uploadId, fileSize, fileSize, crcValue);
        }

        return result;
    }

    void FailOutgoingUpload(
        int uploadId,
        int errorCode,
        const std::string& errorType,
        const std::string& reason,
        int httpStatus = 0)
    {
        {
            std::lock_guard<std::mutex> lock(outgoingMutex);
            auto it = outgoingUploads.find(uploadId);
            if (it != outgoingUploads.end()) {
                it->second.status = UploadStatus::Failed;
                it->second.errorCode = errorCode;
                it->second.errorType = errorType;
                it->second.errorMessage = reason;
                it->second.httpStatus = httpStatus;
            }
        }

        PushOutgoingEvent({
            OutgoingUploadEvent::Type::Failed,
            uploadId, 0, 0, httpStatus, errorCode, errorType, "", reason
        });
    }

    bool ParseUrl(const std::string& url, std::string& scheme, std::string& host, int& port, std::string& path)
    {
        size_t schemePos = url.find("://");
        if (schemePos == std::string::npos) {
            scheme = "http";
            schemePos = 0;
        } else {
            scheme = url.substr(0, schemePos);
            std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            schemePos += 3;
        }

        size_t pathPos = url.find('/', schemePos);
        if (pathPos == std::string::npos) {
            host = url.substr(schemePos);
            path = "/";
        } else {
            host = url.substr(schemePos, pathPos - schemePos);
            path = url.substr(pathPos);
        }

        size_t portPos = host.find(':');
        if (portPos != std::string::npos) {
            try {
                port = std::stoi(host.substr(portPos + 1));
            } catch (...) {
                return false;
            }
            host = host.substr(0, portPos);
        } else {
            port = (scheme == "https") ? 443 : 80;
        }

        return !host.empty();
    }

    template <typename ClientT>
    httplib::Result DispatchOutgoingRequest(
        ClientT& client,
        const OutgoingRequest& request,
        const std::string& path,
        httplib::Headers headers)
    {
        const HttpMethod method = static_cast<HttpMethod>(request.method);
        const bool hasBody = !request.body.empty();

        std::string contentType = request.expectJson ? "application/json" : "text/plain";
        auto contentTypeIt = headers.find("Content-Type");
        if (contentTypeIt != headers.end()) {
            contentType = contentTypeIt->second;
        } else if (hasBody) {
            SetOrReplaceHeader(headers, "Content-Type", contentType);
        }

        switch (method) {
            case HttpMethod::GET:
                return client.Get(path, headers);
            case HttpMethod::HEAD:
                return client.Head(path, headers);
            case HttpMethod::OPTIONS:
                return client.Options(path, headers);
            case HttpMethod::POST:
                return hasBody ? client.Post(path, headers, request.body, contentType)
                               : client.Post(path, headers, "", contentType);
            case HttpMethod::PUT:
                return hasBody ? client.Put(path, headers, request.body, contentType)
                               : client.Put(path, headers, "", contentType);
            case HttpMethod::PATCH:
                return hasBody ? client.Patch(path, headers, request.body, contentType)
                               : client.Patch(path, headers, "", contentType);
            case HttpMethod::DELETE_:
                return hasBody ? client.Delete(path, headers, request.body, contentType)
                               : client.Delete(path, headers);
        }

        return httplib::Result(nullptr, httplib::Error::Unknown);
    }

    void FailOutgoingRequest(
        int requestId,
        int errorCode,
        const std::string& errorType,
        const std::string& reason,
        int httpStatus = 0)
    {
        std::string callbackName;
        {
            std::lock_guard<std::mutex> lock(requestMutex);
            auto it = outgoingRequests.find(requestId);
            if (it == outgoingRequests.end()) return;
            callbackName = it->second.callbackName;
            it->second.status = (errorCode == OutgoingError::CANCELLED)
                ? RequestStatus::Cancelled
                : RequestStatus::Failed;
            it->second.errorCode = errorCode;
            it->second.errorType = errorType;
            it->second.errorMessage = reason;
            it->second.httpStatus = httpStatus;
        }

        PushRequestEvent({
            OutgoingRequestEvent::Type::Failed,
            requestId,
            callbackName,
            httpStatus,
            "",
            -1,
            errorCode,
            errorType,
            reason
        });
    }

    void ProcessOutgoingRequest(int requestId)
    {
        OutgoingRequest request;
        {
            std::lock_guard<std::mutex> lock(requestMutex);
            auto it = outgoingRequests.find(requestId);
            if (it == outgoingRequests.end()) return;
            request = it->second;
        }

        std::string scheme, host, path;
        int port = 80;
        if (!ParseUrl(request.url, scheme, host, port, path)) {
            FailOutgoingRequest(requestId, OutgoingError::INVALID_URL, "invalid_url", "invalid URL");
            return;
        }

        if (scheme != "http" && scheme != "https") {
            FailOutgoingRequest(requestId, OutgoingError::UNSUPPORTED_SCHEME, "unsupported_scheme", "only http:// and https:// are supported");
            return;
        }

        if (IsCancelledRequest(requestId)) {
            FailOutgoingRequest(requestId, OutgoingError::CANCELLED, "cancelled", "request cancelled");
            return;
        }

        httplib::Headers headers;
        for (const auto& [key, value] : SplitHeaderPairs(request.customHeader)) {
            SetOrReplaceHeader(headers, key, value);
        }

        httplib::Result res(nullptr, httplib::Error::Unknown);
        if (scheme == "https") {
#if PAWNREST_HAS_SSL
            httplib::SSLClient client(host, port);
            if (!client.is_valid()) {
                FailOutgoingRequest(requestId, OutgoingError::TLS_INVALID_CERTS, "tls", "failed to initialize TLS client");
                return;
            }
            ConfigureHttpClient(client);
            client.enable_server_certificate_verification(request.verifyTls);
            res = DispatchOutgoingRequest(client, request, path, headers);
#else
            FailOutgoingRequest(requestId, OutgoingError::TLS_UNAVAILABLE, "tls_unavailable", "TLS support is not available in this build");
            return;
#endif
        } else {
            httplib::Client client(host, port);
            ConfigureHttpClient(client);
            res = DispatchOutgoingRequest(client, request, path, headers);
        }

        if (IsCancelledRequest(requestId)) {
            FailOutgoingRequest(requestId, OutgoingError::CANCELLED, "cancelled", "request cancelled");
            return;
        }

        if (!res) {
            auto [errorCode, errorType] = ClassifyClientError(res.error());
            FailOutgoingRequest(
                requestId,
                errorCode,
                errorType,
                "HTTP request failed: " + httplib::to_string(res.error()));
            return;
        }

        int nodeId = -1;
        if (request.expectJson) {
            if (!res->body.empty()) {
                Json::NodePtr responseNode;
                if (!Json::ParseNode(res->body, responseNode)) {
                    FailOutgoingRequest(
                        requestId,
                        OutgoingError::JSON_PARSE,
                        "json_parse",
                        "response body is not valid JSON",
                        res->status);
                    return;
                }
                nodeId = StoreJsonNode(responseNode);
                if (nodeId < 0) {
                    FailOutgoingRequest(
                        requestId,
                        OutgoingError::UNKNOWN,
                        "storage",
                        "failed to store JSON response node",
                        res->status);
                    return;
                }
            } else {
                nodeId = StoreJsonNode(Json::MakeNull());
            }
        }

        {
            std::lock_guard<std::mutex> lock(requestMutex);
            auto it = outgoingRequests.find(requestId);
            if (it != outgoingRequests.end()) {
                it->second.status = RequestStatus::Completed;
                it->second.httpStatus = res->status;
                it->second.responseBody = res->body;
                it->second.errorCode = OutgoingError::NONE;
                it->second.errorType.clear();
                it->second.errorMessage.clear();
            }
        }

        if (request.expectJson) {
            PushRequestEvent({
                OutgoingRequestEvent::Type::CompletedJson,
                requestId,
                request.callbackName,
                res->status,
                "",
                nodeId,
                OutgoingError::NONE,
                "",
                ""
            });
        } else {
            PushRequestEvent({
                OutgoingRequestEvent::Type::CompletedText,
                requestId,
                request.callbackName,
                res->status,
                res->body,
                -1,
                OutgoingError::NONE,
                "",
                ""
            });
        }
    }

    void RequestWorker()
    {
        while (requestWorkerRunning) {
            int requestId = -1;
            {
                std::lock_guard<std::mutex> lock(requestMutex);
                for (auto& [id, request] : outgoingRequests) {
                    if (request.status == RequestStatus::Pending) {
                        request.status = RequestStatus::Requesting;
                        requestId = id;
                        break;
                    }
                }
            }

            if (requestId == -1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            ProcessOutgoingRequest(requestId);
        }
    }

    void WebSocketReadLoop(const std::shared_ptr<WebSocketConnection>& conn)
    {
        if (!conn || !conn->client) return;

        while (!conn->stopToken->load()) {
            std::string payload;
            auto readResult = conn->client->read(payload);
            if (readResult == httplib::ws::ReadResult::Fail) {
                int status = conn->closeStatus;
                if (!conn->stopToken->load() &&
                    status == static_cast<int>(httplib::ws::CloseStatus::Normal)) {
                    status = static_cast<int>(httplib::ws::CloseStatus::Abnormal);
                }
                int errorCode = conn->stopToken->load() ? OutgoingError::NONE : OutgoingError::WEBSOCKET;
                EmitWebSocketDisconnect(conn, status, conn->closeReason, errorCode);
                return;
            }

            if (readResult != httplib::ws::ReadResult::Text &&
                readResult != httplib::ws::ReadResult::Binary) {
                continue;
            }

            if (conn->jsonMode) {
                Json::NodePtr node;
                if (!Json::ParseNode(payload, node)) {
                    conn->closeStatus = static_cast<int>(httplib::ws::CloseStatus::InvalidPayload);
                    conn->closeReason = "invalid json payload";
                    conn->stopToken->store(true);
                    conn->client->close(httplib::ws::CloseStatus::InvalidPayload, conn->closeReason);
                    EmitWebSocketDisconnect(conn, conn->closeStatus, conn->closeReason, OutgoingError::JSON_PARSE);
                    return;
                }

                int nodeId = StoreJsonNode(node);
                if (nodeId < 0) {
                    conn->closeStatus = static_cast<int>(httplib::ws::CloseStatus::InternalError);
                    conn->closeReason = "failed to store json payload";
                    conn->stopToken->store(true);
                    conn->client->close(httplib::ws::CloseStatus::InternalError, conn->closeReason);
                    EmitWebSocketDisconnect(conn, conn->closeStatus, conn->closeReason, OutgoingError::UNKNOWN);
                    return;
                }

                PushWebSocketEvent({
                    WebSocketEvent::Type::MessageJson,
                    conn->socketId,
                    conn->callbackName,
                    true,
                    "",
                    nodeId,
                    static_cast<int>(httplib::ws::CloseStatus::Normal),
                    "",
                    OutgoingError::NONE
                });
            } else {
                PushWebSocketEvent({
                    WebSocketEvent::Type::MessageText,
                    conn->socketId,
                    conn->callbackName,
                    false,
                    payload,
                    -1,
                    static_cast<int>(httplib::ws::CloseStatus::Normal),
                    "",
                    OutgoingError::NONE
                });
            }
        }

        EmitWebSocketDisconnect(conn, conn->closeStatus, conn->closeReason, OutgoingError::NONE);
    }

    void ShutdownWebSocketConnections()
    {
        std::vector<std::shared_ptr<WebSocketConnection>> sockets;
        {
            std::lock_guard<std::mutex> lock(webSocketMutex);
            for (auto& [_, conn] : webSocketConnections) {
                sockets.push_back(conn);
            }
            webSocketConnections.clear();
        }

        for (auto& conn : sockets) {
            if (!conn) continue;
            conn->stopToken->store(true);
            if (conn->client && conn->client->is_open()) {
                conn->client->close(httplib::ws::CloseStatus::Normal, "shutdown");
            }
        }

        for (auto& conn : sockets) {
            if (!conn) continue;
            if (conn->readThread.joinable()) {
                conn->readThread.join();
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // REST API Router
    // ═══════════════════════════════════════════════════════════════════════════

    std::pair<std::regex, std::vector<std::string>> CompileEndpointPattern(const std::string& endpoint)
    {
        std::vector<std::string> paramNames;
        std::string pattern = "^";
        
        size_t i = 0;
        while (i < endpoint.size()) {
            if (endpoint[i] == '{') {
                size_t end = endpoint.find('}', i);
                if (end != std::string::npos) {
                    paramNames.push_back(endpoint.substr(i + 1, end - i - 1));
                    pattern += "([^/]+)";
                    i = end + 1;
                    continue;
                }
            }
            
            // Escape regex special chars
            char c = endpoint[i];
            if (c == '.' || c == '+' || c == '*' || c == '?' || 
                c == '^' || c == '$' || c == '(' || c == ')' ||
                c == '[' || c == ']' || c == '|' || c == '\\') {
                pattern += '\\';
            }
            pattern += c;
            ++i;
        }
        
        pattern += "$";
        return { std::regex(pattern), paramNames };
    }

    std::unordered_map<std::string, std::string> ParseQueryString(const std::string& query)
    {
        std::unordered_map<std::string, std::string> result;
        std::stringstream ss(query);
        std::string pair;
        
        while (std::getline(ss, pair, '&')) {
            size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                std::string key = pair.substr(0, eq);
                std::string val = pair.substr(eq + 1);
                // Basic URL decode
                std::string decoded;
                for (size_t i = 0; i < val.size(); ++i) {
                    if (val[i] == '%' && i + 2 < val.size()) {
                        char hex[3] = { val[i+1], val[i+2], 0 };
                        decoded += static_cast<char>(std::strtol(hex, nullptr, 16));
                        i += 2;
                    } else if (val[i] == '+') {
                        decoded += ' ';
                    } else {
                        decoded += val[i];
                    }
                }
                result[key] = decoded;
            }
        }
        return result;
    }

    APIRoute* MatchApiRoute(HttpMethod method, const std::string& path, std::unordered_map<std::string, std::string>& params)
    {
        std::lock_guard<std::mutex> lock(apiRoutesMutex);
        
        for (auto& kv : apiRoutes) {
            APIRoute& route = kv.second;
            if (route.method != method) continue;
            
            std::smatch match;
            if (std::regex_match(path, match, route.pattern)) {
                params.clear();
                for (size_t i = 0; i < route.paramNames.size() && i + 1 < match.size(); ++i) {
                    params[route.paramNames[i]] = match[i + 1].str();
                }
                return &route;
            }
        }
        return nullptr;
    }

    bool CheckApiAuth(const APIRoute& route, const httplib::Request& req)
    {
        if (!route.requireAuth || route.authKeys.empty()) return true;
        
        auto auth = req.get_header_value("Authorization");
        if (auth.empty()) return false;
        
        if (auth.rfind("Bearer ", 0) == 0) {
            std::string token = auth.substr(7);
            return route.authKeys.count(token) > 0;
        }
        return false;
    }

    std::shared_ptr<RequestContext> CreateRequestContext(
        HttpMethod method,
        const httplib::Request& req,
        httplib::Response& res)
    {
        auto ctx = std::make_shared<RequestContext>();
        ctx->requestId = nextRequestId++;
        ctx->method = method;
        ctx->path = req.path;
        ctx->body = req.body;
        ctx->clientIP = req.remote_addr;
        ctx->httpRes = &res;
        
        // Parse query string directly from request target first (more reliable across route styles).
        auto qPos = req.target.find('?');
        if (qPos != std::string::npos && qPos + 1 < req.target.size()) {
            auto parsed = ParseQueryString(req.target.substr(qPos + 1));
            for (const auto& kv : parsed) {
                ctx->queries[kv.first] = kv.second;
            }
        }

        // Fallback to httplib-provided params when available.
        if (!req.params.empty()) {
            for (const auto& kv : req.params) {
                if (ctx->queries.find(kv.first) == ctx->queries.end()) {
                    ctx->queries[kv.first] = kv.second;
                }
            }
        }
        
        // Copy relevant headers
        for (const auto& kv : req.headers) {
            ctx->headers[kv.first] = kv.second;
            std::string lowered = kv.first;
            for (char& c : lowered) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            ctx->headers[lowered] = kv.second;
        }
        
        return ctx;
    }

    void HandleApiRequest(HttpMethod method, const httplib::Request& req, httplib::Response& res)
    {
        std::string path = req.path;
        std::unordered_map<std::string, std::string> urlParams;
        
        APIRoute* route = MatchApiRoute(method, path, urlParams);
        if (!route && method == HttpMethod::HEAD) {
            route = MatchApiRoute(HttpMethod::GET, path, urlParams);
        }
        if (!route) {
            res.status = 404;
            res.set_content(Json::Obj({
                Json::Bool("success", false),
                Json::Str("error", "Endpoint not found", false)
            }), "application/json");
            return;
        }
        
        if (!CheckApiAuth(*route, req)) {
            res.status = 401;
            res.set_content(Json::Obj({
                Json::Bool("success", false),
                Json::Str("error", "Unauthorized", false)
            }), "application/json");
            return;
        }
        
        auto ctx = CreateRequestContext(method, req, res);
        ctx->params = urlParams;
        
        {
            std::lock_guard<std::mutex> lock(requestsMutex);
            activeRequests[ctx->requestId] = ctx;
        }
        
        // Queue callback event
        {
            std::lock_guard<std::mutex> lock(apiEventMutex);
            pendingApiEvents.push_back({
                ctx->requestId,
                route->routeId,
                route->callbackName
            });
        }
        
        // Wait for response (with timeout)
        auto start = std::chrono::steady_clock::now();
        while (!ctx->responded.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(30)) {
                res.status = 504;
                res.set_content(Json::Obj({
                    Json::Bool("success", false),
                    Json::Str("error", "Gateway timeout", false)
                }), "application/json");
                break;
            }
        }
        
        // Cleanup
        {
            std::lock_guard<std::mutex> lock(requestsMutex);
            activeRequests.erase(ctx->requestId);
        }
    }
    void SetupApiHandlers()
    {
        if (!httpServer) return;
        
        // Generic handlers for all methods
        auto handleGet = [this](const httplib::Request& req, httplib::Response& res) {
            if (req.method == "HEAD") {
                HandleApiRequest(HttpMethod::HEAD, req, res);
            } else {
                HandleApiRequest(HttpMethod::GET, req, res);
            }
        };
        auto handlePost = [this](const httplib::Request& req, httplib::Response& res) {
            HandleApiRequest(HttpMethod::POST, req, res);
        };
        auto handlePut = [this](const httplib::Request& req, httplib::Response& res) {
            HandleApiRequest(HttpMethod::PUT, req, res);
        };
        auto handlePatch = [this](const httplib::Request& req, httplib::Response& res) {
            HandleApiRequest(HttpMethod::PATCH, req, res);
        };
        auto handleDelete = [this](const httplib::Request& req, httplib::Response& res) {
            HandleApiRequest(HttpMethod::DELETE_, req, res);
        };
        auto handleOptions = [this](const httplib::Request& req, httplib::Response& res) {
            HandleApiRequest(HttpMethod::OPTIONS, req, res);
        };
        
        // Register all API routes
        std::lock_guard<std::mutex> lock(apiRoutesMutex);
        for (const auto& kv : apiRoutes) {
            const APIRoute& route = kv.second;
            std::string endpoint = route.endpoint;
            
            // Convert {param} to regex pattern for httplib
            std::string httpPattern = endpoint;
            size_t pos = 0;
            while ((pos = httpPattern.find('{', pos)) != std::string::npos) {
                size_t end = httpPattern.find('}', pos);
                if (end != std::string::npos) {
                    httpPattern.replace(pos, end - pos + 1, "([^/]+)");
                } else {
                    break;
                }
            }
            
            switch (route.method) {
                case HttpMethod::GET:
                    httpServer->Get(httpPattern.c_str(), handleGet);
                    break;
                case HttpMethod::HEAD:
                    httpServer->Get(httpPattern.c_str(), handleGet);
                    break;
                case HttpMethod::POST:
                    httpServer->Post(httpPattern.c_str(), handlePost);
                    break;
                case HttpMethod::PUT:
                    httpServer->Put(httpPattern.c_str(), handlePut);
                    break;
                case HttpMethod::PATCH:
                    httpServer->Patch(httpPattern.c_str(), handlePatch);
                    break;
                case HttpMethod::DELETE_:
                    httpServer->Delete(httpPattern.c_str(), handleDelete);
                    break;
                case HttpMethod::OPTIONS:
                    httpServer->Options(httpPattern.c_str(), handleOptions);
                    break;
            }
        }
        
        // Built-in endpoints
        httpServer->Get("/health", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(Json::Obj({
                Json::Bool("success", true),
                Json::Str("status", "healthy"),
                Json::Num("uptime", static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now().time_since_epoch()
                    ).count()
                ), false)
            }), "application/json");
        });
        
        httpServer->Get("/stats", [this](const httplib::Request&, httplib::Response& res) {
            std::lock_guard<std::mutex> lock1(routesMutex);
            std::lock_guard<std::mutex> lock2(apiRoutesMutex);
            std::lock_guard<std::mutex> lock3(requestsMutex);
            
            res.set_content(Json::Obj({
                Json::Bool("success", true),
                Json::Num("fileRoutes", static_cast<long long>(routes.size())),
                Json::Num("apiRoutes", static_cast<long long>(apiRoutes.size())),
                Json::Num("activeRequests", static_cast<long long>(activeRequests.size()), false)
            }), "application/json");
        });
        
        // File route REST API endpoints
        SetupFileRouteHandlers();
    }
    
    void SetupFileRouteHandlers()
    {
        if (!httpServer) return;
        
        std::lock_guard<std::mutex> lock(routesMutex);
        for (const auto& kv : routes) {
            RegisterFileOpsForRoute(kv.first, kv.second.endpoint);
        }
    }
    
    // Register file ops endpoints for a single route (called when route is created)
    void RegisterFileOpsForRoute(int routeId, const std::string& endpoint)
    {
        if (!httpServer) return;
        
        int capturedId = routeId;
        
        // GET {endpoint}/files - List files
        httpServer->Get((endpoint + "/files").c_str(), 
            [this, capturedId](const httplib::Request& req, httplib::Response& res) {
                HandleFileList(req, res, capturedId);
            });
        
        // GET {endpoint}/files/{filename} - Download file
        httpServer->Get((endpoint + "/files/(.+)").c_str(),
            [this, capturedId](const httplib::Request& req, httplib::Response& res) {
                if (req.path.find("/info") != std::string::npos) return; // Skip if /info
                HandleFileDownload(req, res, capturedId);
            });
        
        // GET {endpoint}/files/{filename}/info - File info
        httpServer->Get((endpoint + "/files/(.+)/info").c_str(),
            [this, capturedId](const httplib::Request& req, httplib::Response& res) {
                HandleFileInfo(req, res, capturedId);
            });
        
        // DELETE {endpoint}/files/{filename} - Delete file
        httpServer->Delete((endpoint + "/files/(.+)").c_str(),
            [this, capturedId](const httplib::Request& req, httplib::Response& res) {
                HandleFileDelete(req, res, capturedId);
            });
    }
    
    bool CheckFileRouteAuth(const UploadRoute& route, const httplib::Request& req)
    {
        if (route.authorizedKeys.empty()) return true;
        
        auto auth = req.get_header_value("Authorization");
        if (auth.empty()) return false;
        
        if (auth.rfind("Bearer ", 0) == 0) {
            std::string token = auth.substr(7);
            return route.authorizedKeys.count(token) > 0;
        }
        return false;
    }
    
    void HandleFileList(const httplib::Request& req, httplib::Response& res, int routeId)
    {
        UploadRoute route;
        {
            std::lock_guard<std::mutex> lock(routesMutex);
            auto it = routes.find(routeId);
            if (it == routes.end()) {
                res.status = 404;
                res.set_content(Json::Obj({ Json::Str("error", "Route not found", false) }), "application/json");
                return;
            }
            route = it->second;
        }
        
        if (!route.allowList) {
            res.status = 403;
            res.set_content(Json::Obj({ Json::Str("error", "File listing not allowed", false) }), "application/json");
            return;
        }
        
        if (!CheckFileRouteAuth(route, req)) {
            res.status = 401;
            res.set_content(Json::Obj({ Json::Str("error", "Unauthorized", false) }), "application/json");
            return;
        }
        
        // Call Pawn callback for permission check
        // For now, just list files
        std::string destPath = serverRootPath + route.destinationPath;
        std::vector<std::string> files;
        
        #ifdef _WIN32
            std::string searchPath = destPath + "*";
            WIN32_FIND_DATAA findData;
            HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        files.push_back(findData.cFileName);
                    }
                } while (FindNextFileA(hFind, &findData));
                FindClose(hFind);
            }
        #else
            DIR* dir = opendir(destPath.c_str());
            if (dir) {
                struct dirent* entry;
                while ((entry = readdir(dir)) != nullptr) {
                    if (entry->d_type == DT_REG) {
                        files.push_back(entry->d_name);
                    }
                }
                closedir(dir);
            }
        #endif
        
        std::string json = "{\"success\":true,\"count\":" + std::to_string(files.size()) + ",\"files\":[";
        for (size_t i = 0; i < files.size(); ++i) {
            if (i > 0) json += ",";
            json += "\"" + Json::Escape(files[i]) + "\"";
        }
        json += "]}";
        
        res.set_content(json, "application/json");
    }
    
    void HandleFileDownload(const httplib::Request& req, httplib::Response& res, int routeId)
    {
        UploadRoute route;
        {
            std::lock_guard<std::mutex> lock(routesMutex);
            auto it = routes.find(routeId);
            if (it == routes.end()) {
                res.status = 404;
                res.set_content(Json::Obj({ Json::Str("error", "Route not found", false) }), "application/json");
                return;
            }
            route = it->second;
        }
        
        if (!route.allowDownload) {
            res.status = 403;
            res.set_content(Json::Obj({ Json::Str("error", "File download not allowed", false) }), "application/json");
            return;
        }
        
        if (!CheckFileRouteAuth(route, req)) {
            res.status = 401;
            res.set_content(Json::Obj({ Json::Str("error", "Unauthorized", false) }), "application/json");
            return;
        }
        
        // Extract filename from path
        std::string filename;
        std::string pathPrefix = route.endpoint + "/files/";
        if (req.path.rfind(pathPrefix, 0) == 0) {
            filename = req.path.substr(pathPrefix.size());
        }
        
        std::string safeName = SanitizeFilename(filename);
        if (safeName.empty()) {
            res.status = 400;
            res.set_content(Json::Obj({ Json::Str("error", "Invalid filename", false) }), "application/json");
            return;
        }
        
        std::string fullPath = serverRootPath + route.destinationPath + safeName;
        if (!FileUtils::FileExists(fullPath)) {
            res.status = 404;
            res.set_content(Json::Obj({ Json::Str("error", "File not found", false) }), "application/json");
            return;
        }
        
        // Read and send file
        std::ifstream file(fullPath, std::ios::binary);
        if (!file) {
            res.status = 500;
            res.set_content(Json::Obj({ Json::Str("error", "Failed to read file", false) }), "application/json");
            return;
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        
        res.set_header("Content-Disposition", "attachment; filename=\"" + safeName + "\"");
        res.set_content(buffer.str(), "application/octet-stream");
    }
    
    void HandleFileInfo(const httplib::Request& req, httplib::Response& res, int routeId)
    {
        UploadRoute route;
        {
            std::lock_guard<std::mutex> lock(routesMutex);
            auto it = routes.find(routeId);
            if (it == routes.end()) {
                res.status = 404;
                res.set_content(Json::Obj({ Json::Str("error", "Route not found", false) }), "application/json");
                return;
            }
            route = it->second;
        }
        
        if (!route.allowInfo) {
            res.status = 403;
            res.set_content(Json::Obj({ Json::Str("error", "File info not allowed", false) }), "application/json");
            return;
        }
        
        if (!CheckFileRouteAuth(route, req)) {
            res.status = 401;
            res.set_content(Json::Obj({ Json::Str("error", "Unauthorized", false) }), "application/json");
            return;
        }
        
        // Extract filename from path
        std::string filename;
        std::string pathPrefix = route.endpoint + "/files/";
        std::string pathSuffix = "/info";
        if (req.path.rfind(pathPrefix, 0) == 0) {
            filename = req.path.substr(pathPrefix.size());
            if (filename.size() > pathSuffix.size() && 
                filename.substr(filename.size() - pathSuffix.size()) == pathSuffix) {
                filename = filename.substr(0, filename.size() - pathSuffix.size());
            }
        }
        
        std::string safeName = SanitizeFilename(filename);
        if (safeName.empty()) {
            res.status = 400;
            res.set_content(Json::Obj({ Json::Str("error", "Invalid filename", false) }), "application/json");
            return;
        }
        
        std::string fullPath = serverRootPath + route.destinationPath + safeName;
        if (!FileUtils::FileExists(fullPath)) {
            res.status = 404;
            res.set_content(Json::Obj({ Json::Str("error", "File not found", false) }), "application/json");
            return;
        }
        
        size_t fileSize = FileUtils::FileSize(fullPath);
        std::int64_t modTime = FileUtils::GetFileModificationTime(fullPath);
        uint32_t crc = CRC32::fileChecksum(fullPath);
        
        res.set_content(Json::Obj({
            Json::Bool("success", true),
            Json::Str("filename", safeName),
            Json::Num("size", static_cast<long long>(fileSize)),
            Json::Num("modified", modTime),
            Json::Str("crc32", CRC32::toHex(crc), false)
        }), "application/json");
    }
    
    void HandleFileDelete(const httplib::Request& req, httplib::Response& res, int routeId)
    {
        UploadRoute route;
        {
            std::lock_guard<std::mutex> lock(routesMutex);
            auto it = routes.find(routeId);
            if (it == routes.end()) {
                res.status = 404;
                res.set_content(Json::Obj({ Json::Str("error", "Route not found", false) }), "application/json");
                return;
            }
            route = it->second;
        }
        
        if (!route.allowDelete) {
            res.status = 403;
            res.set_content(Json::Obj({ Json::Str("error", "File deletion not allowed", false) }), "application/json");
            return;
        }
        
        if (!CheckFileRouteAuth(route, req)) {
            res.status = 401;
            res.set_content(Json::Obj({ Json::Str("error", "Unauthorized", false) }), "application/json");
            return;
        }
        
        // Extract filename from path
        std::string filename;
        std::string pathPrefix = route.endpoint + "/files/";
        if (req.path.rfind(pathPrefix, 0) == 0) {
            filename = req.path.substr(pathPrefix.size());
        }
        
        std::string safeName = SanitizeFilename(filename);
        if (safeName.empty()) {
            res.status = 400;
            res.set_content(Json::Obj({ Json::Str("error", "Invalid filename", false) }), "application/json");
            return;
        }
        
        std::string fullPath = serverRootPath + route.destinationPath + safeName;
        if (!FileUtils::FileExists(fullPath)) {
            res.status = 404;
            res.set_content(Json::Obj({ Json::Str("error", "File not found", false) }), "application/json");
            return;
        }
        
        if (FileUtils::RemoveFile(fullPath)) {
            res.set_content(Json::Obj({
                Json::Bool("success", true),
                Json::Str("message", "File deleted"),
                Json::Str("filename", safeName, false)
            }), "application/json");
        } else {
            res.status = 500;
            res.set_content(Json::Obj({ Json::Str("error", "Failed to delete file", false) }), "application/json");
        }
    }

    bool StartHttpServer(
        int port,
        bool useTls = false,
        const std::string& certPath = "",
        const std::string& keyPath = "")
    {
        if (isRunning) {
            PrintLn("[PawnREST] ERROR: Server already running on port %d", currentPort);
            return false;
        }

        tlsEnabled = false;
        tlsCertPath.clear();
        tlsKeyPath.clear();

        if (useTls) {
#if PAWNREST_HAS_SSL
            auto resolvePath = [this](const std::string& input) -> std::string {
                std::string normalized = NormalizeSlashes(Trim(input));
                if (normalized.empty()) return "";
#ifdef _WIN32
                if (normalized.size() >= 2 &&
                    std::isalpha(static_cast<unsigned char>(normalized[0])) &&
                    normalized[1] == ':') {
                    return normalized;
                }
#endif
                if (!normalized.empty() && normalized[0] == '/') {
                    return normalized;
                }
                return serverRootPath + normalized;
            };

            std::string certFullPath = resolvePath(certPath);
            std::string keyFullPath = resolvePath(keyPath);
            if (certFullPath.empty() || keyFullPath.empty()) {
                PrintLn("[PawnREST] ERROR: TLS requires valid cert and key paths");
                return false;
            }
            if (!FileUtils::FileExists(certFullPath) || !FileUtils::FileExists(keyFullPath)) {
                PrintLn("[PawnREST] ERROR: TLS cert/key file not found");
                return false;
            }

            auto sslServer = std::make_unique<httplib::SSLServer>(certFullPath.c_str(), keyFullPath.c_str());
            if (!sslServer->is_valid()) {
                PrintLn("[PawnREST] ERROR: Failed to initialize HTTPS server with provided cert/key");
                return false;
            }
            httpServer = std::move(sslServer);
            tlsEnabled = true;
            tlsCertPath = certFullPath;
            tlsKeyPath = keyFullPath;
#else
            PrintLn("[PawnREST] ERROR: TLS/HTTPS is not available in this build");
            return false;
#endif
        } else {
            httpServer = std::make_unique<httplib::Server>();
        }

        httpServer->set_error_handler([](const httplib::Request&, httplib::Response& res) {
            if (res.status == 404) {
                res.set_content(Json::Obj({
                    Json::Str("error", "endpoint not found", false)
                }), "application/json");
            }
        });

        {
            std::unique_lock<std::mutex> lock(routesMutex);
            for (const auto& kv : routes) {
                // FIX: avoid structured binding capture bug in C++17
                // copy the id and endpoint to local variables before capturing
                int capturedId = kv.first;
                std::string capturedEndpoint = kv.second.endpoint;

                httpServer->Post(capturedEndpoint.c_str(),
                    [this, capturedId](
                        const httplib::Request& req,
                        httplib::Response& res,
                        const httplib::ContentReader& content_reader)
                    {
                        HandleUploadStreaming(req, res, content_reader, capturedId);
                    }
                );
            }
        }

        // Setup REST API handlers
        SetupApiHandlers();

        if (!httpServer->bind_to_port("0.0.0.0", port)) {
            PrintLn("[PawnREST] ERROR: Port %d is already in use. Please use a different port.", port);
            httpServer.reset();
            tlsEnabled = false;
            tlsCertPath.clear();
            tlsKeyPath.clear();
            return false;
        }

        currentPort = port;
        isRunning = true;

        httpThread = std::thread([this]() {
            httpServer->listen_after_bind();
        });

        PrintLn("  [PawnREST] %s server started on port %d", tlsEnabled ? "HTTPS" : "HTTP", port);
        return true;
    }

    void StopHttpServer()
    {
        if (httpServer) {
            httpServer->stop();
        }
        if (httpThread.joinable()) {
            httpThread.join();
        }
        if (httpServer) {
            httpServer.reset();
        }
        isRunning = false;
        currentPort = 0;
        tlsEnabled = false;
        tlsCertPath.clear();
        tlsKeyPath.clear();
    }

public:


public:
    PawnRESTCore() = default;

    void SetLogger(void (*sink)(const char*))
    {
        logSink = sink;
    }

    void Initialize()
    {
        if (serverRootPath.empty()) {
            serverRootPath = FileUtils::GetCurrentWorkingDirectory();
        }
        FileUtils::CreateDirectory(serverRootPath + Config::TEMP_DIR);

        if (!uploadWorkerRunning) {
            uploadWorkerRunning = true;
            uploadWorkerThread = std::thread([this]() { UploadWorker(); });
        }

        if (!requestWorkerRunning) {
            requestWorkerRunning = true;
            requestWorkerThread = std::thread([this]() { RequestWorker(); });
        }

        if (!cleanupThread.joinable()) {
            shutdownFlag = false;
            cleanupThread = std::thread([this]() {
                while (!shutdownFlag) {
                    for (int i = 0; i < 60 && !shutdownFlag; ++i) {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                    if (shutdownFlag) break;

                    try {
                        FileUtils::CleanupTempFiles(
                            serverRootPath + Config::TEMP_DIR,
                            static_cast<int>(Config::TEMP_CLEANUP_SEC)
                        );
                    } catch (...) {}
                }
            });
        }
    }

    void Shutdown()
    {
        StopHttpServer();

        uploadWorkerRunning = false;
        if (uploadWorkerThread.joinable()) uploadWorkerThread.join();

        requestWorkerRunning = false;
        if (requestWorkerThread.joinable()) requestWorkerThread.join();

        ShutdownWebSocketConnections();

        shutdownFlag = true;
        if (cleanupThread.joinable()) cleanupThread.join();
    }

    ~PawnRESTCore()
    {
        Shutdown();
    }

    // Public API
    bool Start(int port) { return StartHttpServer(port, false); }
    bool StartTLS(int port, const std::string& certPath, const std::string& keyPath)
    {
        return StartHttpServer(port, true, certPath, keyPath);
    }
    bool Stop() {
        if (!isRunning) return false;
        bool wasTls = tlsEnabled;
        StopHttpServer();
        PrintLn("  [PawnREST] %s server stopped", wasTls ? "HTTPS" : "HTTP");
        return true;
    }
    bool IsRunning() const { return isRunning; }
    int GetPort() const { return currentPort; }
    bool IsTLSEnabled() const { return tlsEnabled; }

    // ═══════════════════════════════════════════════════════════════════════
    // RECEIVE API
    // ═══════════════════════════════════════════════════════════════════════
    int RegisterRoute(
        const std::string& endpoint,
        const std::string& path,
        const std::string& allowedExts,
        int maxSizeMb)
    {
        if (endpoint.empty() || path.empty()) return -1;
        if (endpoint[0] != '/') return -1;

        std::string safePath = SanitizeRelativeDir(path);
        if (safePath.empty()) return -1;

        UploadRoute route;
        route.routeId         = nextRouteId++;
        route.endpoint        = endpoint;
        route.destinationPath = safePath;
        route.allowedExtensions = SplitCSV(allowedExts);
        route.maxSizeBytes      = static_cast<size_t>(std::max(maxSizeMb, 1)) * 1024 * 1024;
        route.requireCrc32      = false;

        FileUtils::CreateDirectory(serverRootPath + route.destinationPath);

        int id = route.routeId;
        std::string capturedEndpoint = route.endpoint;

        std::unique_lock<std::mutex> lock(routesMutex);
        routes[id] = std::move(route);
        lock.unlock();

        if (isRunning && httpServer) {
            // FIX: capture id and endpoint as local copies, not from structured binding
            int capturedId = id;
            httpServer->Post(capturedEndpoint.c_str(),
                [this, capturedId](
                    const httplib::Request& req,
                    httplib::Response& res,
                    const httplib::ContentReader& content_reader)
                {
                    HandleUploadStreaming(req, res, content_reader, capturedId);
                }
            );
            
            // Register file ops endpoints (list, download, delete, info)
            RegisterFileOpsForRoute(id, capturedEndpoint);
        }

        PrintLn("  Files: POST %s -> %s", capturedEndpoint.c_str(), safePath.c_str());
        return id;
    }

    bool AddKeyToRoute(int routeId, const std::string& key)
    {
        std::unique_lock<std::mutex> lock(routesMutex);
        auto it = routes.find(routeId);
        if (it == routes.end()) return false;
        it->second.authorizedKeys.insert(key);
        return true;
    }

    bool RemoveKeyFromRoute(int routeId, const std::string& key)
    {
        std::unique_lock<std::mutex> lock(routesMutex);
        auto it = routes.find(routeId);
        if (it == routes.end()) return false;
        it->second.authorizedKeys.erase(key);
        return true;
    }

    bool SetConflictMode(int routeId, int mode)
    {
        std::unique_lock<std::mutex> lock(routesMutex);
        auto it = routes.find(routeId);
        if (it == routes.end()) return false;

        switch (mode) {
            case 0: it->second.onConflict = ConflictMode::Rename;    break;
            case 1: it->second.onConflict = ConflictMode::Overwrite; break;
            case 2: it->second.onConflict = ConflictMode::Reject;    break;
            default: return false;
        }
        return true;
    }

    bool SetCorruptAction(int routeId, int action)
    {
        std::unique_lock<std::mutex> lock(routesMutex);
        auto it = routes.find(routeId);
        if (it == routes.end()) return false;

        switch (action) {
            case 0: it->second.onCorrupt = CorruptAction::Delete;     break;
            case 1: it->second.onCorrupt = CorruptAction::Quarantine; break;
            case 2: it->second.onCorrupt = CorruptAction::Keep;       break;
            default: return false;
        }
        return true;
    }

    bool SetRequireCRC32(int routeId, bool required)
    {
        std::unique_lock<std::mutex> lock(routesMutex);
        auto it = routes.find(routeId);
        if (it == routes.end()) return false;
        it->second.requireCrc32 = required;
        return true;
    }

    bool RemoveRoute(int routeId)
    {
        std::unique_lock<std::mutex> lock(routesMutex);
        auto it = routes.find(routeId);
        if (it == routes.end()) return false;
        routes.erase(it);
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // REST API
    // ═══════════════════════════════════════════════════════════════════════
    
    int RegisterApiRoute(int method, const std::string& endpoint, const std::string& callback)
    {
        if (endpoint.empty() || callback.empty()) return -1;
        if (endpoint[0] != '/') return -1;
        if (method < 0 || method > 6) return -1;
        
        APIRoute route;
        route.routeId = nextApiRouteId++;
        route.method = static_cast<HttpMethod>(method);
        route.endpoint = endpoint;
        route.callbackName = callback;
        route.requireAuth = false;
        
        auto compiled = CompileEndpointPattern(endpoint);
        route.pattern = compiled.first;
        route.paramNames = compiled.second;
        
        int id = route.routeId;
        
        {
            std::lock_guard<std::mutex> lock(apiRoutesMutex);
            apiRoutes[id] = std::move(route);
        }
        
        // If server is already running, we need to add the handler
        if (isRunning && httpServer) {
            std::string httpPattern = endpoint;
            size_t pos = 0;
            while ((pos = httpPattern.find('{', pos)) != std::string::npos) {
                size_t end = httpPattern.find('}', pos);
                if (end != std::string::npos) {
                    httpPattern.replace(pos, end - pos + 1, "([^/]+)");
                } else {
                    break;
                }
            }
            
            switch (static_cast<HttpMethod>(method)) {
                case HttpMethod::GET:
                    httpServer->Get(httpPattern.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
                        if (req.method == "HEAD") {
                            HandleApiRequest(HttpMethod::HEAD, req, res);
                        } else {
                            HandleApiRequest(HttpMethod::GET, req, res);
                        }
                    });
                    break;
                case HttpMethod::HEAD:
                    httpServer->Get(httpPattern.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
                        if (req.method == "HEAD") {
                            HandleApiRequest(HttpMethod::HEAD, req, res);
                        } else {
                            HandleApiRequest(HttpMethod::GET, req, res);
                        }
                    });
                    break;
                case HttpMethod::POST:
                    httpServer->Post(httpPattern.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
                        HandleApiRequest(HttpMethod::POST, req, res);
                    });
                    break;
                case HttpMethod::PUT:
                    httpServer->Put(httpPattern.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
                        HandleApiRequest(HttpMethod::PUT, req, res);
                    });
                    break;
                case HttpMethod::PATCH:
                    httpServer->Patch(httpPattern.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
                        HandleApiRequest(HttpMethod::PATCH, req, res);
                    });
                    break;
                case HttpMethod::DELETE_:
                    httpServer->Delete(httpPattern.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
                        HandleApiRequest(HttpMethod::DELETE_, req, res);
                    });
                    break;
                case HttpMethod::OPTIONS:
                    httpServer->Options(httpPattern.c_str(), [this](const httplib::Request& req, httplib::Response& res) {
                        HandleApiRequest(HttpMethod::OPTIONS, req, res);
                    });
                    break;
            }
        }
        
        const char* methodNames[] = { "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS" };
        PrintLn("  API: %s %s -> %s()", methodNames[method], endpoint.c_str(), callback.c_str());
        return id;
    }
    
    bool RemoveApiRoute(int routeId)
    {
        std::lock_guard<std::mutex> lock(apiRoutesMutex);
        auto it = apiRoutes.find(routeId);
        if (it == apiRoutes.end()) return false;
        apiRoutes.erase(it);
        return true;
    }
    
    bool SetApiRouteAuth(int routeId, const std::string& key)
    {
        std::lock_guard<std::mutex> lock(apiRoutesMutex);
        auto it = apiRoutes.find(routeId);
        if (it == apiRoutes.end()) return false;
        it->second.authKeys.insert(key);
        it->second.requireAuth = true;
        return true;
    }
    
    // File route permissions
    bool SetAllowList(int routeId, bool allow)
    {
        std::lock_guard<std::mutex> lock(routesMutex);
        auto it = routes.find(routeId);
        if (it == routes.end()) return false;
        it->second.allowList = allow;
        return true;
    }
    
    bool SetAllowDownload(int routeId, bool allow)
    {
        std::lock_guard<std::mutex> lock(routesMutex);
        auto it = routes.find(routeId);
        if (it == routes.end()) return false;
        it->second.allowDownload = allow;
        return true;
    }
    
    bool SetAllowDelete(int routeId, bool allow)
    {
        std::lock_guard<std::mutex> lock(routesMutex);
        auto it = routes.find(routeId);
        if (it == routes.end()) return false;
        it->second.allowDelete = allow;
        return true;
    }
    
    bool SetAllowInfo(int routeId, bool allow)
    {
        std::lock_guard<std::mutex> lock(routesMutex);
        auto it = routes.find(routeId);
        if (it == routes.end()) return false;
        it->second.allowInfo = allow;
        return true;
    }
    
    // File operations
    int GetRouteFileCount(int routeId)
    {
        std::string destPath;
        {
            std::lock_guard<std::mutex> lock(routesMutex);
            auto it = routes.find(routeId);
            if (it == routes.end()) return -1;
            destPath = serverRootPath + it->second.destinationPath;
        }
        
        int count = 0;
        #ifdef _WIN32
            std::string searchPath = destPath + "*";
            WIN32_FIND_DATAA findData;
            HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        ++count;
                    }
                } while (FindNextFileA(hFind, &findData));
                FindClose(hFind);
            }
        #else
            DIR* dir = opendir(destPath.c_str());
            if (dir) {
                struct dirent* entry;
                while ((entry = readdir(dir)) != nullptr) {
                    if (entry->d_type == DT_REG) {
                        ++count;
                    }
                }
                closedir(dir);
            }
        #endif
        return count;
    }
    
    std::string GetRouteFileName(int routeId, int index)
    {
        std::string destPath;
        {
            std::lock_guard<std::mutex> lock(routesMutex);
            auto it = routes.find(routeId);
            if (it == routes.end()) return "";
            destPath = serverRootPath + it->second.destinationPath;
        }
        
        int count = 0;
        std::string result;
        
        #ifdef _WIN32
            std::string searchPath = destPath + "*";
            WIN32_FIND_DATAA findData;
            HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        if (count == index) {
                            result = findData.cFileName;
                            break;
                        }
                        ++count;
                    }
                } while (FindNextFileA(hFind, &findData));
                FindClose(hFind);
            }
        #else
            DIR* dir = opendir(destPath.c_str());
            if (dir) {
                struct dirent* entry;
                while ((entry = readdir(dir)) != nullptr) {
                    if (entry->d_type == DT_REG) {
                        if (count == index) {
                            result = entry->d_name;
                            break;
                        }
                        ++count;
                    }
                }
                closedir(dir);
            }
        #endif
        return result;
    }
    
    bool DeleteRouteFile(int routeId, const std::string& filename)
    {
        std::string destPath;
        {
            std::lock_guard<std::mutex> lock(routesMutex);
            auto it = routes.find(routeId);
            if (it == routes.end()) return false;
            destPath = serverRootPath + it->second.destinationPath;
        }
        
        std::string safeName = SanitizeFilename(filename);
        if (safeName.empty()) return false;
        
        std::string fullPath = destPath + safeName;
        return FileUtils::RemoveFile(fullPath);
    }
    
    size_t GetRouteFileSize(int routeId, const std::string& filename)
    {
        std::string destPath;
        {
            std::lock_guard<std::mutex> lock(routesMutex);
            auto it = routes.find(routeId);
            if (it == routes.end()) return 0;
            destPath = serverRootPath + it->second.destinationPath;
        }
        
        std::string safeName = SanitizeFilename(filename);
        if (safeName.empty()) return 0;
        
        std::string fullPath = destPath + safeName;
        return FileUtils::FileSize(fullPath);
    }
    
    // Request data access
    std::shared_ptr<RequestContext> GetRequest(int requestId)
    {
        std::lock_guard<std::mutex> lock(requestsMutex);
        auto it = activeRequests.find(requestId);
        if (it == activeRequests.end()) return nullptr;
        return it->second;
    }
    
    std::string GetRequestIP(int requestId)
    {
        auto ctx = GetRequest(requestId);
        return ctx ? ctx->clientIP : "";
    }
    
    int GetRequestMethod(int requestId)
    {
        auto ctx = GetRequest(requestId);
        return ctx ? static_cast<int>(ctx->method) : -1;
    }
    
    std::string GetRequestPath(int requestId)
    {
        auto ctx = GetRequest(requestId);
        return ctx ? ctx->path : "";
    }
    
    std::string GetRequestBody(int requestId)
    {
        auto ctx = GetRequest(requestId);
        return ctx ? ctx->body : "";
    }
    
    int GetRequestBodyLength(int requestId)
    {
        auto ctx = GetRequest(requestId);
        return ctx ? static_cast<int>(ctx->body.size()) : 0;
    }
    
    std::string GetParam(int requestId, const std::string& name)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx) return "";
        auto it = ctx->params.find(name);
        return (it != ctx->params.end()) ? it->second : "";
    }
    
    int GetParamInt(int requestId, const std::string& name)
    {
        std::string val = GetParam(requestId, name);
        return val.empty() ? 0 : std::atoi(val.c_str());
    }
    
    std::string GetQuery(int requestId, const std::string& name)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx) return "";
        auto it = ctx->queries.find(name);
        return (it != ctx->queries.end()) ? it->second : "";
    }
    
    int GetQueryInt(int requestId, const std::string& name, int defaultVal)
    {
        std::string val = GetQuery(requestId, name);
        return val.empty() ? defaultVal : std::atoi(val.c_str());
    }
    
    std::string GetHeader(int requestId, const std::string& name)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx) return "";
        auto it = ctx->headers.find(name);
        if (it != ctx->headers.end()) return it->second;

        std::string lowered = name;
        for (char& c : lowered) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        it = ctx->headers.find(lowered);
        return (it != ctx->headers.end()) ? it->second : "";
    }
    
    // JSON parsing from request body
    std::string JsonGetString(int requestId, const std::string& key, const std::string& def)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx) return def;
        return Json::GetString(ctx->body, key, def);
    }
    
    int JsonGetInt(int requestId, const std::string& key, int def)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx) return def;
        return Json::GetInt(ctx->body, key, def);
    }
    
    double JsonGetFloat(int requestId, const std::string& key, double def)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx) return def;
        return Json::GetFloat(ctx->body, key, def);
    }
    
    bool JsonGetBool(int requestId, const std::string& key, bool def)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx) return def;
        return Json::GetBool(ctx->body, key, def);
    }
    
    bool JsonHasKey(int requestId, const std::string& key)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx) return false;
        return Json::HasKey(ctx->body, key);
    }
    
    int JsonArrayLength(int requestId, const std::string& key)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx) return -1;
        return Json::ArrayLength(ctx->body, key);
    }
    
    std::string JsonGetNested(int requestId, const std::string& path, const std::string& def)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx) return def;
        return Json::GetNestedString(ctx->body, path, def);
    }
    
    int JsonGetNestedInt(int requestId, const std::string& path, int def)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx) return def;
        return Json::GetNestedInt(ctx->body, path, def);
    }

    int StoreJsonNode(const Json::NodePtr& node)
    {
        if (!node) return -1;
        std::lock_guard<std::mutex> lock(jsonNodesMutex);
        int nodeId = nextJsonNodeId++;
        jsonNodes[nodeId] = node;
        return nodeId;
    }

    Json::NodePtr GetJsonNode(int nodeId)
    {
        std::lock_guard<std::mutex> lock(jsonNodesMutex);
        auto it = jsonNodes.find(nodeId);
        if (it == jsonNodes.end()) return nullptr;
        return it->second;
    }

    bool ReleaseJsonNode(int nodeId)
    {
        std::lock_guard<std::mutex> lock(jsonNodesMutex);
        return jsonNodes.erase(nodeId) > 0;
    }

    int ParseJsonNode(const std::string& json)
    {
        Json::NodePtr root;
        if (!Json::ParseNode(json, root)) return -1;
        return StoreJsonNode(root);
    }

    int ParseRequestJsonNode(int requestId)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx) return -1;
        return ParseJsonNode(ctx->body);
    }

    int JsonNodeObject()
    {
        return StoreJsonNode(Json::MakeObject());
    }

    int JsonNodeArray()
    {
        return StoreJsonNode(Json::MakeArray());
    }

    int JsonNodeString(const std::string& value)
    {
        return StoreJsonNode(Json::MakeString(value));
    }

    int JsonNodeInt(int value)
    {
        return StoreJsonNode(Json::MakeNumber(static_cast<double>(value)));
    }

    int JsonNodeFloat(double value)
    {
        return StoreJsonNode(Json::MakeNumber(value));
    }

    int JsonNodeBool(bool value)
    {
        return StoreJsonNode(Json::MakeBoolean(value));
    }

    int JsonNodeNull()
    {
        return StoreJsonNode(Json::MakeNull());
    }

    int JsonNodeType(int nodeId)
    {
        auto node = GetJsonNode(nodeId);
        if (!node) return -1;
        return static_cast<int>(node->type);
    }

    bool JsonNodeStringify(int nodeId, std::string& output, int maxLen)
    {
        auto node = GetJsonNode(nodeId);
        if (!node) {
            output.clear();
            return false;
        }

        std::string json = Json::StringifyNode(node);
        if (maxLen > 0 && json.length() > static_cast<size_t>(maxLen - 1)) {
            output = json.substr(0, maxLen - 1);
        } else {
            output = json;
        }
        return true;
    }

    bool JsonNodeSet(int objectNodeId, const std::string& key, int valueNodeId)
    {
        auto objectNode = GetJsonNode(objectNodeId);
        auto valueNode = GetJsonNode(valueNodeId);
        if (!objectNode || !valueNode || objectNode->type != Json::NodeType::Object || key.empty()) return false;
        return Json::SetObjectMember(objectNode, key, Json::Clone(valueNode));
    }

    bool JsonNodeSetString(int objectNodeId, const std::string& key, const std::string& value)
    {
        auto objectNode = GetJsonNode(objectNodeId);
        if (!objectNode || objectNode->type != Json::NodeType::Object || key.empty()) return false;
        return Json::SetObjectMember(objectNode, key, Json::MakeString(value));
    }

    bool JsonNodeSetInt(int objectNodeId, const std::string& key, int value)
    {
        auto objectNode = GetJsonNode(objectNodeId);
        if (!objectNode || objectNode->type != Json::NodeType::Object || key.empty()) return false;
        return Json::SetObjectMember(objectNode, key, Json::MakeNumber(static_cast<double>(value)));
    }

    bool JsonNodeSetFloat(int objectNodeId, const std::string& key, double value)
    {
        auto objectNode = GetJsonNode(objectNodeId);
        if (!objectNode || objectNode->type != Json::NodeType::Object || key.empty()) return false;
        return Json::SetObjectMember(objectNode, key, Json::MakeNumber(value));
    }

    bool JsonNodeSetBool(int objectNodeId, const std::string& key, bool value)
    {
        auto objectNode = GetJsonNode(objectNodeId);
        if (!objectNode || objectNode->type != Json::NodeType::Object || key.empty()) return false;
        return Json::SetObjectMember(objectNode, key, Json::MakeBoolean(value));
    }

    bool JsonNodeSetNull(int objectNodeId, const std::string& key)
    {
        auto objectNode = GetJsonNode(objectNodeId);
        if (!objectNode || objectNode->type != Json::NodeType::Object || key.empty()) return false;
        return Json::SetObjectMember(objectNode, key, Json::MakeNull());
    }

    bool JsonNodeHas(int objectNodeId, const std::string& key)
    {
        auto objectNode = GetJsonNode(objectNodeId);
        if (!objectNode || objectNode->type != Json::NodeType::Object) return false;
        return Json::GetObjectMember(objectNode, key) != nullptr;
    }

    int JsonNodeGet(int objectNodeId, const std::string& key)
    {
        auto objectNode = GetJsonNode(objectNodeId);
        if (!objectNode || objectNode->type != Json::NodeType::Object) return -1;
        auto child = Json::GetObjectMember(objectNode, key);
        if (!child) return -1;
        return StoreJsonNode(child);
    }

    std::string JsonNodeGetString(int objectNodeId, const std::string& key, const std::string& def)
    {
        auto objectNode = GetJsonNode(objectNodeId);
        if (!objectNode || objectNode->type != Json::NodeType::Object) return def;
        auto child = Json::GetObjectMember(objectNode, key);
        if (!child || child->type != Json::NodeType::String) return def;
        return child->stringValue;
    }

    int JsonNodeGetInt(int objectNodeId, const std::string& key, int def)
    {
        auto objectNode = GetJsonNode(objectNodeId);
        if (!objectNode || objectNode->type != Json::NodeType::Object) return def;
        auto child = Json::GetObjectMember(objectNode, key);
        if (!child || child->type != Json::NodeType::Number) return def;
        return static_cast<int>(child->numberValue);
    }

    double JsonNodeGetFloat(int objectNodeId, const std::string& key, double def)
    {
        auto objectNode = GetJsonNode(objectNodeId);
        if (!objectNode || objectNode->type != Json::NodeType::Object) return def;
        auto child = Json::GetObjectMember(objectNode, key);
        if (!child || child->type != Json::NodeType::Number) return def;
        return child->numberValue;
    }

    bool JsonNodeGetBool(int objectNodeId, const std::string& key, bool def)
    {
        auto objectNode = GetJsonNode(objectNodeId);
        if (!objectNode || objectNode->type != Json::NodeType::Object) return def;
        auto child = Json::GetObjectMember(objectNode, key);
        if (!child || child->type != Json::NodeType::Boolean) return def;
        return child->boolValue;
    }

    int JsonNodeArrayLength(int arrayNodeId)
    {
        auto arrayNode = GetJsonNode(arrayNodeId);
        if (!arrayNode || arrayNode->type != Json::NodeType::Array) return -1;
        return static_cast<int>(arrayNode->arrayValue.size());
    }

    int JsonNodeArrayGet(int arrayNodeId, int index)
    {
        auto arrayNode = GetJsonNode(arrayNodeId);
        if (!arrayNode || arrayNode->type != Json::NodeType::Array) return -1;
        if (index < 0 || index >= static_cast<int>(arrayNode->arrayValue.size())) return -1;
        return StoreJsonNode(arrayNode->arrayValue[static_cast<size_t>(index)]);
    }

    bool JsonNodeArrayPush(int arrayNodeId, int valueNodeId)
    {
        auto arrayNode = GetJsonNode(arrayNodeId);
        auto valueNode = GetJsonNode(valueNodeId);
        if (!arrayNode || !valueNode || arrayNode->type != Json::NodeType::Array) return false;
        arrayNode->arrayValue.push_back(Json::Clone(valueNode));
        return true;
    }

    bool JsonNodeArrayPushString(int arrayNodeId, const std::string& value)
    {
        auto arrayNode = GetJsonNode(arrayNodeId);
        if (!arrayNode || arrayNode->type != Json::NodeType::Array) return false;
        arrayNode->arrayValue.push_back(Json::MakeString(value));
        return true;
    }

    bool JsonNodeArrayPushInt(int arrayNodeId, int value)
    {
        auto arrayNode = GetJsonNode(arrayNodeId);
        if (!arrayNode || arrayNode->type != Json::NodeType::Array) return false;
        arrayNode->arrayValue.push_back(Json::MakeNumber(static_cast<double>(value)));
        return true;
    }

    bool JsonNodeArrayPushFloat(int arrayNodeId, double value)
    {
        auto arrayNode = GetJsonNode(arrayNodeId);
        if (!arrayNode || arrayNode->type != Json::NodeType::Array) return false;
        arrayNode->arrayValue.push_back(Json::MakeNumber(value));
        return true;
    }

    bool JsonNodeArrayPushBool(int arrayNodeId, bool value)
    {
        auto arrayNode = GetJsonNode(arrayNodeId);
        if (!arrayNode || arrayNode->type != Json::NodeType::Array) return false;
        arrayNode->arrayValue.push_back(Json::MakeBoolean(value));
        return true;
    }

    bool JsonNodeArrayPushNull(int arrayNodeId)
    {
        auto arrayNode = GetJsonNode(arrayNodeId);
        if (!arrayNode || arrayNode->type != Json::NodeType::Array) return false;
        arrayNode->arrayValue.push_back(Json::MakeNull());
        return true;
    }

    int JsonAppend(int leftNodeId, int rightNodeId)
    {
        auto leftNode = GetJsonNode(leftNodeId);
        auto rightNode = GetJsonNode(rightNodeId);
        if (!leftNode || !rightNode) return -1;
        if (leftNode->type != rightNode->type) return -1;

        Json::NodePtr merged;
        if (leftNode->type == Json::NodeType::Object) {
            merged = Json::Clone(leftNode);
            for (const auto& [key, value] : rightNode->objectValue) {
                Json::SetObjectMember(merged, key, Json::Clone(value));
            }
        } else if (leftNode->type == Json::NodeType::Array) {
            merged = Json::Clone(leftNode);
            for (const auto& value : rightNode->arrayValue) {
                merged->arrayValue.push_back(Json::Clone(value));
            }
        } else {
            return -1;
        }

        int mergedId = StoreJsonNode(merged);
        if (mergedId < 0) return -1;

        ReleaseJsonNode(leftNodeId);
        if (rightNodeId != leftNodeId) {
            ReleaseJsonNode(rightNodeId);
        }
        return mergedId;
    }

    bool RespondNode(int requestId, int status, int nodeId)
    {
        auto node = GetJsonNode(nodeId);
        if (!node) return false;
        return RespondJSON(requestId, status, Json::StringifyNode(node));
    }
    
    // Response methods
    bool Respond(int requestId, int status, const std::string& body, const std::string& contentType)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx || ctx->responded.load(std::memory_order_acquire) || !ctx->httpRes) return false;
        
        for (const auto& kv : ctx->responseHeaders) {
            ctx->httpRes->set_header(kv.first.c_str(), kv.second.c_str());
        }
        
        ctx->httpRes->status = status;
        ctx->httpRes->set_content(body, contentType.c_str());
        ctx->responded.store(true, std::memory_order_release);
        return true;
    }
    
    bool RespondJSON(int requestId, int status, const std::string& json)
    {
        return Respond(requestId, status, json, "application/json");
    }
    
    bool RespondError(int requestId, int status, const std::string& message)
    {
        return RespondJSON(requestId, status, Json::Obj({
            Json::Bool("success", false),
            Json::Str("error", message, false)
        }));
    }
    
    bool SetResponseHeader(int requestId, const std::string& name, const std::string& value)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx || ctx->responded.load(std::memory_order_acquire)) return false;
        ctx->responseHeaders[name] = value;
        return true;
    }
    
    // JSON builder for responses
    bool JsonStart(int requestId)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx || ctx->jsonStarted) return false;
        ctx->responseJson = "{";
        ctx->jsonStarted = true;
        ctx->jsonStack.clear();
        ctx->jsonStack.push_back("object");
        return true;
    }
    
    bool JsonAddString(int requestId, const std::string& key, const std::string& value)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx || !ctx->jsonStarted) return false;
        if (ctx->responseJson.back() != '{' && ctx->responseJson.back() != '[' && ctx->responseJson.back() != ',')
            ctx->responseJson += ",";
        if (!ctx->jsonStack.empty() && ctx->jsonStack.back() == "object" && !key.empty()) {
            ctx->responseJson += "\"" + Json::Escape(key) + "\":";
        }
        ctx->responseJson += "\"" + Json::Escape(value) + "\"";
        return true;
    }
    
    bool JsonAddInt(int requestId, const std::string& key, int value)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx || !ctx->jsonStarted) return false;
        if (ctx->responseJson.back() != '{' && ctx->responseJson.back() != '[' && ctx->responseJson.back() != ',')
            ctx->responseJson += ",";
        if (!ctx->jsonStack.empty() && ctx->jsonStack.back() == "object" && !key.empty()) {
            ctx->responseJson += "\"" + Json::Escape(key) + "\":";
        }
        ctx->responseJson += std::to_string(value);
        return true;
    }
    
    bool JsonAddFloat(int requestId, const std::string& key, double value)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx || !ctx->jsonStarted) return false;
        if (ctx->responseJson.back() != '{' && ctx->responseJson.back() != '[' && ctx->responseJson.back() != ',')
            ctx->responseJson += ",";
        if (!ctx->jsonStack.empty() && ctx->jsonStack.back() == "object" && !key.empty()) {
            ctx->responseJson += "\"" + Json::Escape(key) + "\":";
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6f", value);
        ctx->responseJson += buf;
        return true;
    }
    
    bool JsonAddBool(int requestId, const std::string& key, bool value)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx || !ctx->jsonStarted) return false;
        if (ctx->responseJson.back() != '{' && ctx->responseJson.back() != '[' && ctx->responseJson.back() != ',')
            ctx->responseJson += ",";
        if (!ctx->jsonStack.empty() && ctx->jsonStack.back() == "object" && !key.empty()) {
            ctx->responseJson += "\"" + Json::Escape(key) + "\":";
        }
        ctx->responseJson += value ? "true" : "false";
        return true;
    }
    
    bool JsonAddNull(int requestId, const std::string& key)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx || !ctx->jsonStarted) return false;
        if (ctx->responseJson.back() != '{' && ctx->responseJson.back() != '[' && ctx->responseJson.back() != ',')
            ctx->responseJson += ",";
        if (!ctx->jsonStack.empty() && ctx->jsonStack.back() == "object" && !key.empty()) {
            ctx->responseJson += "\"" + Json::Escape(key) + "\":";
        }
        ctx->responseJson += "null";
        return true;
    }
    
    bool JsonStartObject(int requestId, const std::string& key)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx || !ctx->jsonStarted) return false;
        if (ctx->responseJson.back() != '{' && ctx->responseJson.back() != '[' && ctx->responseJson.back() != ',')
            ctx->responseJson += ",";
        if (!ctx->jsonStack.empty() && ctx->jsonStack.back() == "object" && !key.empty()) {
            ctx->responseJson += "\"" + Json::Escape(key) + "\":";
        }
        ctx->responseJson += "{";
        ctx->jsonStack.push_back("object");
        return true;
    }
    
    bool JsonEndObject(int requestId)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx || !ctx->jsonStarted || ctx->jsonStack.empty()) return false;
        if (ctx->jsonStack.back() != "object") return false;
        ctx->jsonStack.pop_back();
        ctx->responseJson += "}";
        return true;
    }
    
    bool JsonStartArray(int requestId, const std::string& key)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx || !ctx->jsonStarted) return false;
        if (ctx->responseJson.back() != '{' && ctx->responseJson.back() != '[' && ctx->responseJson.back() != ',')
            ctx->responseJson += ",";
        if (!ctx->jsonStack.empty() && ctx->jsonStack.back() == "object" && !key.empty()) {
            ctx->responseJson += "\"" + Json::Escape(key) + "\":";
        }
        ctx->responseJson += "[";
        ctx->jsonStack.push_back("array");
        return true;
    }
    
    bool JsonEndArray(int requestId)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx || !ctx->jsonStarted || ctx->jsonStack.empty()) return false;
        if (ctx->jsonStack.back() != "array") return false;
        ctx->jsonStack.pop_back();
        ctx->responseJson += "]";
        return true;
    }
    
    bool JsonSend(int requestId, int status)
    {
        auto ctx = GetRequest(requestId);
        if (!ctx || !ctx->jsonStarted) return false;
        
        // Close all open structures
        while (!ctx->jsonStack.empty()) {
            if (ctx->jsonStack.back() == "object") {
                ctx->responseJson += "}";
            } else {
                ctx->responseJson += "]";
            }
            ctx->jsonStack.pop_back();
        }
        
        return RespondJSON(requestId, status, ctx->responseJson);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // UPLOAD API (Client)
    // ═══════════════════════════════════════════════════════════════════════
    int QueueUpload(
        const std::string& url,
        const std::string& filepath,
        const std::string& filename,
        const std::string& authKey,
        const std::string& customHeaders,
        bool calculateCrc32,
        int mode,
        bool verifyTls = true)
    {
        if (url.empty() || filepath.empty()) return -1;

        std::string safeFile = NormalizeSlashes(filepath);
        if (safeFile.find("..") != std::string::npos) return -1;
        if (!safeFile.empty() && safeFile[0] == '/') return -1;

        std::string fullPath = serverRootPath + safeFile;
        if (!FileUtils::FileExists(fullPath)) {
            PrintLn("[PawnREST] Upload failed: file not found %s", fullPath.c_str());
            return -1;
        }

        OutgoingUpload upload;
        upload.uploadId = nextOutgoingId++;
        upload.url = url;
        upload.filepath = safeFile;
        upload.filename = filename.empty() ? SanitizeFilename(filepath) : SanitizeFilename(filename);
        upload.authKey = authKey;
        upload.customHeader = customHeaders;
        upload.calculateCrc32 = calculateCrc32;
        upload.status = UploadStatus::Pending;
        upload.totalBytes = FileUtils::FileSize(fullPath);
        upload.mode = (mode == 1) ? UploadMode::Raw : UploadMode::Multipart;
        upload.verifyTls = verifyTls;

        int id = upload.uploadId;

        {
            std::lock_guard<std::mutex> lock(outgoingMutex);
            outgoingUploads[id] = std::move(upload);
        }

        PrintLn("[PawnREST] Upload queued #%d: %s -> %s (%s)",
            id, filepath.c_str(), url.c_str(), (mode == 1 ? "RAW" : "MULTIPART"));
        return id;
    }

    int CreateUploadClient(const std::string& baseUrl, const std::string& defaultHeaders, bool verifyTls)
    {
        if (baseUrl.empty()) return -1;

        std::string scheme, host, path;
        int port = 80;
        if (!ParseUrl(baseUrl, scheme, host, port, path)) return -1;
        if (scheme != "http" && scheme != "https") return -1;

        UploadClient client;
        client.clientId = nextUploadClientId++;
        client.baseUrl = baseUrl;
        client.defaultHeaders = ParseHeaderMap(defaultHeaders);
        client.verifyTls = verifyTls;

        int id = client.clientId;
        {
            std::lock_guard<std::mutex> lock(outgoingMutex);
            uploadClients[id] = std::move(client);
        }
        PrintLn("[PawnREST] Upload client #%d created: %s", id, baseUrl.c_str());
        return id;
    }

    bool RemoveUploadClient(int clientId)
    {
        std::lock_guard<std::mutex> lock(outgoingMutex);
        auto it = uploadClients.find(clientId);
        if (it == uploadClients.end()) return false;
        uploadClients.erase(it);
        return true;
    }

    bool SetUploadClientHeader(int clientId, const std::string& name, const std::string& value)
    {
        if (name.empty()) return false;
        std::lock_guard<std::mutex> lock(outgoingMutex);
        auto it = uploadClients.find(clientId);
        if (it == uploadClients.end()) return false;
        it->second.defaultHeaders[name] = value;
        return true;
    }

    bool RemoveUploadClientHeader(int clientId, const std::string& name)
    {
        if (name.empty()) return false;
        std::lock_guard<std::mutex> lock(outgoingMutex);
        auto it = uploadClients.find(clientId);
        if (it == uploadClients.end()) return false;
        return it->second.defaultHeaders.erase(name) > 0;
    }

    int QueueUploadWithClient(
        int clientId,
        const std::string& path,
        const std::string& filepath,
        const std::string& filename,
        const std::string& authKey,
        const std::string& customHeaders,
        bool calculateCrc32,
        int mode)
    {
        UploadClient client;
        {
            std::lock_guard<std::mutex> lock(outgoingMutex);
            auto it = uploadClients.find(clientId);
            if (it == uploadClients.end()) return -1;
            client = it->second;
        }

        std::string fullUrl = JoinUrlPath(client.baseUrl, path);

        auto mergedHeaders = client.defaultHeaders;
        for (const auto& [key, value] : SplitHeaderPairs(customHeaders)) {
            mergedHeaders[key] = value;
        }

        return QueueUpload(
            fullUrl,
            filepath,
            filename,
            authKey,
            HeaderMapToString(mergedHeaders),
            calculateCrc32,
            mode,
            client.verifyTls
        );
    }

    bool CancelUpload(int uploadId)
    {
        std::lock_guard<std::mutex> lock(outgoingMutex);
        auto it = outgoingUploads.find(uploadId);
        if (it == outgoingUploads.end()) return false;

        if (it->second.status == UploadStatus::Completed ||
            it->second.status == UploadStatus::Failed ||
            it->second.status == UploadStatus::Cancelled) {
            return false;
        }

        it->second.status = UploadStatus::Cancelled;
        if (it->second.cancelToken) {
            it->second.cancelToken->store(true);
        }
        return true;
    }

    int GetUploadStatus(int uploadId)
    {
        std::lock_guard<std::mutex> lock(outgoingMutex);
        auto it = outgoingUploads.find(uploadId);
        if (it == outgoingUploads.end()) return -1;
        return static_cast<int>(it->second.status);
    }

    int GetUploadProgress(int uploadId)
    {
        std::lock_guard<std::mutex> lock(outgoingMutex);
        auto it = outgoingUploads.find(uploadId);
        if (it == outgoingUploads.end()) return -1;
        return it->second.progressPct;
    }

    bool GetUploadResponse(int uploadId, std::string& output, int maxLen)
    {
        std::lock_guard<std::mutex> lock(outgoingMutex);
        auto it = outgoingUploads.find(uploadId);
        if (it == outgoingUploads.end()) {
            output = "";
            return false;
        }

        const std::string& resp = it->second.responseBody;
        if (maxLen > 0 && resp.length() > static_cast<size_t>(maxLen - 1)) {
            output = resp.substr(0, maxLen - 1);
        } else {
            output = resp;
        }
        return it->second.status == UploadStatus::Completed;
    }

    int GetUploadErrorCode(int uploadId)
    {
        std::lock_guard<std::mutex> lock(outgoingMutex);
        auto it = outgoingUploads.find(uploadId);
        if (it == outgoingUploads.end()) return -1;
        return it->second.errorCode;
    }

    int GetUploadHttpStatus(int uploadId)
    {
        std::lock_guard<std::mutex> lock(outgoingMutex);
        auto it = outgoingUploads.find(uploadId);
        if (it == outgoingUploads.end()) return 0;
        return it->second.httpStatus;
    }

    bool GetUploadErrorType(int uploadId, std::string& output, int maxLen)
    {
        std::lock_guard<std::mutex> lock(outgoingMutex);
        auto it = outgoingUploads.find(uploadId);
        if (it == outgoingUploads.end()) {
            output.clear();
            return false;
        }

        const std::string& errorType = it->second.errorType;
        if (maxLen > 0 && errorType.length() > static_cast<size_t>(maxLen - 1)) {
            output = errorType.substr(0, maxLen - 1);
        } else {
            output = errorType;
        }
        return !errorType.empty();
    }

    // ═══════════════════════════════════════════════════════════════════════
    // OUTBOUND REQUEST API (Requests-style)
    // ═══════════════════════════════════════════════════════════════════════
    int CreateRequestClient(const std::string& baseUrl, const std::string& defaultHeaders, bool verifyTls)
    {
        return CreateUploadClient(baseUrl, defaultHeaders, verifyTls);
    }

    bool RemoveRequestClient(int clientId)
    {
        return RemoveUploadClient(clientId);
    }

    bool SetRequestClientHeader(int clientId, const std::string& name, const std::string& value)
    {
        return SetUploadClientHeader(clientId, name, value);
    }

    bool RemoveRequestClientHeader(int clientId, const std::string& name)
    {
        return RemoveUploadClientHeader(clientId, name);
    }

    int QueueOutboundRequest(
        int clientId,
        const std::string& path,
        int method,
        const std::string& callback,
        const std::string& body,
        const std::string& customHeaders,
        bool expectJson)
    {
        if (callback.empty()) return -1;
        if (method < static_cast<int>(HttpMethod::GET) || method > static_cast<int>(HttpMethod::OPTIONS)) return -1;

        UploadClient client;
        {
            std::lock_guard<std::mutex> lock(outgoingMutex);
            auto it = uploadClients.find(clientId);
            if (it == uploadClients.end()) return -1;
            client = it->second;
        }

        std::string fullUrl = JoinUrlPath(client.baseUrl, path);
        std::string scheme, host, parsedPath;
        int port = 80;
        if (!ParseUrl(fullUrl, scheme, host, port, parsedPath)) return -1;
        if (scheme != "http" && scheme != "https") return -1;

        auto mergedHeaders = client.defaultHeaders;
        for (const auto& [key, value] : SplitHeaderPairs(customHeaders)) {
            mergedHeaders[key] = value;
        }

        OutgoingRequest request;
        request.requestId = nextOutgoingRequestId++;
        request.url = fullUrl;
        request.method = method;
        request.callbackName = callback;
        request.body = body;
        request.customHeader = HeaderMapToString(mergedHeaders);
        request.expectJson = expectJson;
        request.verifyTls = client.verifyTls;
        request.status = RequestStatus::Pending;

        int requestId = request.requestId;
        {
            std::lock_guard<std::mutex> lock(requestMutex);
            outgoingRequests[requestId] = std::move(request);
        }

        return requestId;
    }

    bool CancelOutboundRequest(int requestId)
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        auto it = outgoingRequests.find(requestId);
        if (it == outgoingRequests.end()) return false;

        if (it->second.status == RequestStatus::Completed ||
            it->second.status == RequestStatus::Failed ||
            it->second.status == RequestStatus::Cancelled) {
            return false;
        }

        it->second.status = RequestStatus::Cancelled;
        if (it->second.cancelToken) {
            it->second.cancelToken->store(true);
        }
        return true;
    }

    int GetOutboundRequestStatus(int requestId)
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        auto it = outgoingRequests.find(requestId);
        if (it == outgoingRequests.end()) return -1;
        return static_cast<int>(it->second.status);
    }

    int GetOutboundRequestHttpStatus(int requestId)
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        auto it = outgoingRequests.find(requestId);
        if (it == outgoingRequests.end()) return 0;
        return it->second.httpStatus;
    }

    int GetOutboundRequestErrorCode(int requestId)
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        auto it = outgoingRequests.find(requestId);
        if (it == outgoingRequests.end()) return -1;
        return it->second.errorCode;
    }

    bool GetOutboundRequestErrorType(int requestId, std::string& output, int maxLen)
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        auto it = outgoingRequests.find(requestId);
        if (it == outgoingRequests.end()) {
            output.clear();
            return false;
        }

        const std::string& value = it->second.errorType;
        if (maxLen > 0 && value.length() > static_cast<size_t>(maxLen - 1)) {
            output = value.substr(0, maxLen - 1);
        } else {
            output = value;
        }
        return !value.empty();
    }

    bool GetOutboundRequestResponse(int requestId, std::string& output, int maxLen)
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        auto it = outgoingRequests.find(requestId);
        if (it == outgoingRequests.end()) {
            output.clear();
            return false;
        }

        const std::string& value = it->second.responseBody;
        if (maxLen > 0 && value.length() > static_cast<size_t>(maxLen - 1)) {
            output = value.substr(0, maxLen - 1);
        } else {
            output = value;
        }

        return it->second.status == RequestStatus::Completed;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // WEBSOCKET CLIENT API
    // ═══════════════════════════════════════════════════════════════════════
    void CleanupWebSocketConnection(int socketId)
    {
        std::shared_ptr<WebSocketConnection> conn;
        {
            std::lock_guard<std::mutex> lock(webSocketMutex);
            auto it = webSocketConnections.find(socketId);
            if (it == webSocketConnections.end()) return;
            conn = it->second;
            webSocketConnections.erase(it);
        }

        if (conn && conn->readThread.joinable() &&
            conn->readThread.get_id() != std::this_thread::get_id()) {
            conn->readThread.join();
        }
    }

    int ConnectWebSocketClient(
        const std::string& address,
        const std::string& callback,
        bool jsonMode,
        const std::string& customHeaders,
        bool verifyTls)
    {
        if (address.empty() || callback.empty()) return -1;

        std::string scheme, host, path;
        int port = 80;
        if (!ParseUrl(address, scheme, host, port, path)) return -1;
        if (scheme != "ws" && scheme != "wss") return -1;
#if !PAWNREST_HAS_SSL
        if (scheme == "wss") return -1;
#endif

        httplib::Headers headers;
        for (const auto& [key, value] : SplitHeaderPairs(customHeaders)) {
            SetOrReplaceHeader(headers, key, value);
        }

        auto conn = std::make_shared<WebSocketConnection>();
        conn->socketId = nextWebSocketId++;
        conn->address = address;
        conn->callbackName = callback;
        conn->jsonMode = jsonMode;
        conn->verifyTls = verifyTls;
        conn->headers = ParseHeaderMap(customHeaders);

        conn->client = std::make_unique<httplib::ws::WebSocketClient>(address, headers);
        if (!conn->client || !conn->client->is_valid()) return -1;

        conn->client->set_connection_timeout(Config::UPLOAD_TIMEOUT_SEC);
        conn->client->set_read_timeout(Config::UPLOAD_TIMEOUT_SEC);
        conn->client->set_write_timeout(Config::UPLOAD_TIMEOUT_SEC);
#if PAWNREST_HAS_SSL
        conn->client->enable_server_certificate_verification(verifyTls);
#endif

        if (!conn->client->connect()) {
            return -1;
        }

        int socketId = conn->socketId;
        {
            std::lock_guard<std::mutex> lock(webSocketMutex);
            webSocketConnections[socketId] = conn;
        }

        conn->readThread = std::thread([this, conn]() {
            WebSocketReadLoop(conn);
        });

        return socketId;
    }

    bool CloseWebSocketClient(int socketId, int status, const std::string& reason)
    {
        std::shared_ptr<WebSocketConnection> conn;
        {
            std::lock_guard<std::mutex> lock(webSocketMutex);
            auto it = webSocketConnections.find(socketId);
            if (it == webSocketConnections.end()) return false;
            conn = it->second;
        }

        conn->closeStatus = status;
        conn->closeReason = reason;
        conn->stopToken->store(true);

        if (conn->client && conn->client->is_open()) {
            conn->client->close(static_cast<httplib::ws::CloseStatus>(status), reason);
        }

        EmitWebSocketDisconnect(conn, status, reason, OutgoingError::NONE);
        CleanupWebSocketConnection(socketId);
        return true;
    }

    bool RemoveWebSocketClient(int socketId)
    {
        return CloseWebSocketClient(
            socketId,
            static_cast<int>(httplib::ws::CloseStatus::Normal),
            "closed");
    }

    bool IsWebSocketOpen(int socketId)
    {
        std::lock_guard<std::mutex> lock(webSocketMutex);
        auto it = webSocketConnections.find(socketId);
        if (it == webSocketConnections.end() || !it->second || !it->second->client) return false;
        return it->second->client->is_open();
    }

    bool WebSocketSendText(int socketId, const std::string& data)
    {
        std::shared_ptr<WebSocketConnection> conn;
        {
            std::lock_guard<std::mutex> lock(webSocketMutex);
            auto it = webSocketConnections.find(socketId);
            if (it == webSocketConnections.end()) return false;
            conn = it->second;
        }

        if (!conn || !conn->client || !conn->client->is_open()) return false;
        std::lock_guard<std::mutex> sendLock(conn->sendMutex);
        return conn->client->send(data);
    }

    bool WebSocketSendJson(int socketId, int nodeId)
    {
        auto node = GetJsonNode(nodeId);
        if (!node) return false;
        return WebSocketSendText(socketId, Json::StringifyNode(node));
    }

    // Raw event drain helpers for non-open.mp runtimes (e.g. SA-MP plugin mode).
    std::vector<APIRequestEvent> DrainApiEventsRaw()
    {
        std::vector<APIRequestEvent> events;
        std::lock_guard<std::mutex> lock(apiEventMutex);
        events.swap(pendingApiEvents);
        return events;
    }

    std::vector<UploadEvent> DrainUploadEventsRaw()
    {
        std::vector<UploadEvent> events;
        std::lock_guard<std::mutex> lock(eventMutex);
        events.swap(pendingEvents);
        return events;
    }

    std::vector<OutgoingUploadEvent> DrainOutgoingUploadEventsRaw()
    {
        std::vector<OutgoingUploadEvent> events;
        std::lock_guard<std::mutex> lock(outgoingEventMutex);
        events.swap(pendingOutgoingEvents);
        return events;
    }

    std::vector<OutgoingRequestEvent> DrainRequestEventsRaw()
    {
        std::vector<OutgoingRequestEvent> events;
        std::lock_guard<std::mutex> lock(requestEventMutex);
        events.swap(pendingRequestEvents);
        return events;
    }

    std::vector<WebSocketEvent> DrainWebSocketEventsRaw()
    {
        std::vector<WebSocketEvent> events;
        std::lock_guard<std::mutex> lock(webSocketEventMutex);
        events.swap(pendingWebSocketEvents);
        return events;
    }
};
