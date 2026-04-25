# PawnREST - HTTP/S File Transfer & REST API Framework

An plugin that provides HTTP/S file upload/download functionality and a full REST API framework for SA-MP/open.mp servers.

## Wiki

API documentation and use-case guides are available in [`wiki/`](./wiki/):

## Example Pawn Scripts

Examples Pawn scripts are available in [`example/`](./example/):
- `01_server_routes.pwn` - inbound custom REST routes
- `02_file_routes_and_ops.pwn` - managed file routes and route-backed file operations
- `03_json_nodes.pwn` - JSON node creation, mutation, and response usage
- `04_outbound_uploads.pwn` - outbound file uploads with progress callbacks
- `05_outbound_requests.pwn` - reusable HTTP/S clients for text and JSON requests
- `06_websocket_client.pwn` - text and JSON websocket clients
- `07_crc_utils.pwn` - CRC32 helpers and file comparison utilities
- `08_request_input_fallbacks.pwn` - path/query/header/body input fallback patterns
- `09_discord_webhook.pwn` - Discord webhook notifications using `REST_RequestJSON`

## Features

- **File Upload Server** - Receive files via HTTP POST with validation
- **File Download API** - Serve files via HTTP GET
- **Outgoing Uploads** - Upload files to external servers
- **Upload Clients** - Reuse base URL and default headers for outbound uploads
- **Outbound HTTP Requests** - `REST_CreateRequestClient` with `REST_Request` / `REST_RequestJSON`
- **WebSocket Clients** - Text and JSON websocket clients (`ws://` and optional `wss://`)
- **REST API Framework** - Create custom HTTP endpoints (GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS)
- **Robust Request Accessors** - Query parsing from full URL target and case-insensitive header lookups
- **Node-only JSON API** - Parse/build JSON with node handles
- **Authorization** - Bearer token authentication per route
- **TLS/HTTPS Support** - HTTPS server/client support when built with OpenSSL
- **Structured Error Callbacks** - Typed/coded failure metadata for outgoing uploads
- **CRC32 Integrity** - File checksum validation

---

## Installation

1. Download the latest release for your platform (`.dll` for Windows, `.so` for Linux)
2. Place it in your open.mp server `components/` directory
3. Copy `PawnREST.inc` to your Pawn compiler includes directory
4. Add `#include <PawnREST>` to your script

Public API uses two prefixes: `REST_*` for HTTP/core features and `FILE_*` for file/upload features.

---

## Quick Start

```pawn
#include <open.mp>
#include <PawnREST>

new g_MapRoute = -1;
new g_ApiPlayers = -1;

public OnGameModeInit()
{
    // Start HTTP server on port 8080
    REST_Start(8080);
    
    // === FILE UPLOAD ROUTE ===
    g_MapRoute = FILE_RegisterRoute("/maps", "scriptfiles/maps/", ".map,.json", 50);
    FILE_AddAuthKey(g_MapRoute, "upload-secret-key");
    
    // Enable REST API for files
    FILE_AllowList(g_MapRoute, true);
    FILE_AllowDownload(g_MapRoute, true);
    
    // === CUSTOM REST API ===
    g_ApiPlayers = REST_RegisterAPIRoute(HTTP_METHOD_GET, "/api/players", "OnGetPlayers");
    REST_SetRouteAuthKey(g_ApiPlayers, "api-secret-key");
    
    return 1;
}

// Handle GET /api/players
public OnGetPlayers(requestId)
{
    new payload = JsonObject();
    JsonSetInt(payload, "online", GetPlayerCount());
    JsonSetInt(payload, "max", GetMaxPlayers());
    RespondNode(requestId, 200, payload);
    JsonCleanup(payload);
    return 1;
}

public OnIncomingUploadCompleted(uploadId, routeId, const endpoint[], const filename[], 
                      const filepath[], const crc32[], crcMatched)
{
    printf("[PawnREST] File uploaded: %s", filename);
    return 1;
}
```

---

## HTTP Methods

| Constant | Method |
|----------|--------|
| `HTTP_METHOD_GET` | GET |
| `HTTP_METHOD_POST` | POST |
| `HTTP_METHOD_PUT` | PUT |
| `HTTP_METHOD_PATCH` | PATCH |
| `HTTP_METHOD_DELETE` | DELETE |
| `HTTP_METHOD_HEAD` | HEAD |
| `HTTP_METHOD_OPTIONS` | OPTIONS |

---

## Server Control

```pawn
// Start/stop HTTP server
native bool:REST_Start(port);
native bool:REST_StartTLS(port, const certPath[], const keyPath[]);
native bool:REST_Stop();
native REST_IsRunning();
native REST_GetPort();
native REST_IsTLSEnabled();
```

---

## File Upload Routes

```pawn
// Register upload endpoint
native FILE_RegisterRoute(const endpoint[], const path[], const allowedExts[], maxSizeMb);

// Authorization
native bool:FILE_AddAuthKey(routeId, const key[]);
native bool:FILE_RemoveAuthKey(routeId, const key[]);

// Settings
native bool:FILE_SetConflict(routeId, mode);      // CONFLICT_RENAME/OVERWRITE/REJECT
native bool:FILE_SetCorruptAction(routeId, action);  // CORRUPT_DELETE/QUARANTINE/KEEP
native bool:FILE_SetRequireCRC32(routeId, bool:required);
native bool:FILE_RemoveRoute(routeId);

// REST API permissions for file routes
native bool:FILE_AllowList(routeId, bool:allow);     // GET {route}/files
native bool:FILE_AllowDownload(routeId, bool:allow); // GET {route}/files/{name}
native bool:FILE_AllowDelete(routeId, bool:allow);   // DELETE {route}/files/{name}
native bool:FILE_AllowInfo(routeId, bool:allow);     // GET {route}/files/{name}/info

// File operations
native FILE_GetCount(routeId);
native FILE_GetName(routeId, index, output[], outputSize);
native bool:FILE_Delete(routeId, const filename[]);
native FILE_GetSize(routeId, const filename[]);
```

---

## Custom REST API Routes

```pawn
// Register custom endpoint
native REST_RegisterAPIRoute(method, const endpoint[], const callback[]);
native bool:REST_RemoveAPIRoute(routeId);
native bool:REST_SetRouteAuthKey(routeId, const key[]);

// Examples:
REST_RegisterAPIRoute(HTTP_METHOD_GET, "/api/server", "OnGetServer");
REST_RegisterAPIRoute(HTTP_METHOD_POST, "/api/ban", "OnPostBan");
REST_RegisterAPIRoute(HTTP_METHOD_GET, "/api/player/{id}", "OnGetPlayer");  // URL params
REST_RegisterAPIRoute(HTTP_METHOD_DELETE, "/api/vehicle/{id}", "OnDeleteVehicle");
```

---

## Request Data Access

```pawn
// Basic request info
native REST_GetRequestIP(requestId, output[], outputSize);
native REST_GetRequestMethod(requestId);
native REST_GetRequestPath(requestId, output[], outputSize);
native REST_GetRequestBody(requestId, output[], outputSize);
native REST_GetRequestBodyLength(requestId);

// URL parameters ({id} from /api/player/{id})
native REST_GetParam(requestId, const name[], output[], outputSize);
native REST_GetParamInt(requestId, const name[]);

// Query string (?page=1&limit=10)
native REST_GetQuery(requestId, const name[], output[], outputSize);
native REST_GetQueryInt(requestId, const name[], defaultValue = 0);

// Headers
native REST_GetHeader(requestId, const name[], output[], outputSize);
```

Notes:
- `REST_GetQuery*` reads query params from the full request target and falls back to parsed params.
- `REST_GetHeader` lookups are case-insensitive (`X-Token` and `x-token` are equivalent).

---

## JSON API (Node-only)

```pawn
// Parse JSON
native GetRequestJsonNode(requestId);
native JsonParse(const json[]);
native JsonNodeType(nodeId);                          // JSON_NODE_*
native JsonStringify(nodeId, output[], outputSize);
native bool:JsonCleanup(nodeId);

// Constructors
native JsonObject(...);  // key, node pairs
native JsonArray(...);   // node list
native JsonString(const value[]);
native JsonInt(value);
native JsonFloat(Float:value);
native JsonBool(bool:value);
native JsonNull();
native JsonAppend(leftNodeId, rightNodeId);

// Object operations
native bool:JsonSetObject(objectNodeId, const key[], valueNodeId);
native bool:JsonSetString(objectNodeId, const key[], const value[]);
native bool:JsonSetInt(objectNodeId, const key[], value);
native bool:JsonSetFloat(objectNodeId, const key[], Float:value);
native bool:JsonSetBool(objectNodeId, const key[], bool:value);
native bool:JsonSetNull(objectNodeId, const key[]);
native bool:JsonHas(objectNodeId, const key[]);
native JsonGetNode(objectNodeId, const key[]);
native JsonGetString(objectNodeId, const key[], output[], outputSize);
native JsonGetInt(objectNodeId, const key[], defaultValue = 0);
native Float:JsonGetFloat(objectNodeId, const key[], Float:defaultValue = 0.0);
native bool:JsonGetBool(objectNodeId, const key[], bool:defaultValue = false);

// Array operations
native JsonArrayLength(arrayNodeId);
native JsonArrayGetNode(arrayNodeId, index);
native bool:JsonArrayAppend(arrayNodeId, valueNodeId);
native bool:JsonArrayAppendString(arrayNodeId, const value[]);
native bool:JsonArrayAppendInt(arrayNodeId, value);
native bool:JsonArrayAppendFloat(arrayNodeId, Float:value);
native bool:JsonArrayAppendBool(arrayNodeId, bool:value);
native bool:JsonArrayAppendNull(arrayNodeId);
```

Builder style:
```pawn
new payload = JsonObject(
    "name", JsonString("PawnREST"),
    "players", JsonArray(
        JsonObject("id", JsonInt(0), "name", JsonString("Alice")),
        JsonObject("id", JsonInt(1), "name", JsonString("Bob"))
    )
);
```

## Response Methods

```pawn
native bool:Respond(requestId, status, const body[], const contentType[] = "application/json");
native bool:RespondJSON(requestId, status, const json[]);
native bool:RespondNode(requestId, status, nodeId);
native bool:RespondError(requestId, status, const message[]);
native bool:SetResponseHeader(requestId, const name[], const value[]);
```

---

## Outgoing Uploads

```pawn
// Upload file to external server
native FILE_Upload(
    const url[],
    const filepath[],
    const filename[] = "",
    const authKey[] = "",
    const customHeaders[] = "",
    calculateCrc32 = 1,
    mode = UPLOAD_MODE_MULTIPART,
    bool:verifyTls = true
);

// Reusable upload clients
native FILE_CreateUploadClient(const baseUrl[], const defaultHeaders[] = "", bool:verifyTls = true);
native bool:FILE_RemoveUploadClient(clientId);
native bool:FILE_SetUploadClientHeader(clientId, const name[], const value[]);
native bool:FILE_RemoveUploadClientHeader(clientId, const name[]);
native FILE_UploadWithClient(clientId, const path[], const filepath[], const filename[] = "", const authKey[] = "", const customHeaders[] = "", calculateCrc32 = 1, mode = UPLOAD_MODE_MULTIPART);

native bool:FILE_CancelUpload(uploadId);
native FILE_GetUploadStatus(uploadId);
native FILE_GetUploadProgress(uploadId);
native FILE_GetUploadResponse(uploadId, output[], outputSize);
native FILE_GetUploadErrorCode(uploadId);
native FILE_GetUploadErrorType(uploadId, output[], outputSize);
native FILE_GetUploadHttpStatus(uploadId);
```

---

## Outbound Requests

```pawn
// Reusable HTTP client
native REST_CreateRequestClient(const baseUrl[], const defaultHeaders[] = "", bool:verifyTls = true);
native bool:REST_RemoveRequestsClient(clientId);
native bool:REST_SetRequestsClientHeader(clientId, const name[], const value[]);
native bool:REST_RemoveRequestsClientHeader(clientId, const name[]);

// Async requests
native REST_Request(clientId, const path[], method, const callback[], const body[] = "", const headers[] = "");
native REST_RequestJSON(clientId, const path[], method, const callback[], jsonNodeId = -1, const headers[] = "");

// Optional state polling
native bool:REST_CancelRequest(requestId);
native REST_GetRequestStatus(requestId);      // REQUEST_*
native REST_GetRequestHttpStatus(requestId);
native REST_GetRequestErrorCode(requestId);   // PAWNREST_ERR_*
native REST_GetRequestErrorType(requestId, output[], outputSize);
native REST_GetRequestResponse(requestId, output[], outputSize);
```

**Callback signatures**
```pawn
// REST_Request(...)
public OnTextResponse(requestId, httpStatus, const data[], dataLen)

// REST_RequestJSON(...)
public OnJsonResponse(requestId, httpStatus, nodeId)

// transport/internal failures
forward OnRequestFailure(requestId, errorCode, const errorType[], const errorMessage[], httpStatus);
```

---

## Header Helper Stocks

```pawn
stock bool:REST_RequestHeaders(output[], outputSize, const key[], const value[]);
stock bool:REST_RequestHeadersAppend(headers[], outputSize, const key[], const value[]);
```

---

## WebSocket Client

```pawn
native REST_WebSocketClient(const address[], const callback[], const headers[] = "", bool:verifyTls = true);
native REST_JsonWebSocketClient(const address[], const callback[], const headers[] = "", bool:verifyTls = true);
native bool:REST_WebSocketSend(socketId, const data[]);
native bool:REST_JsonWebSocketSend(socketId, nodeId);
native bool:REST_WebSocketClose(socketId, status = 1000, const reason[] = "");
native bool:REST_RemoveWebSocketClient(socketId);
native bool:REST_IsWebSocketOpen(socketId);
```

**Callback signatures**
```pawn
// REST_WebSocketClient
public OnSocketText(socketId, const data[], dataLen)

// REST_JsonWebSocketClient
public OnSocketJson(socketId, nodeId)

forward OnWebSocketDisconnect(socketId, bool:isJson, status, const reason[], reasonLen, errorCode);
```

---

## CRC32 Utilities

```pawn
native FILE_VerifyCRC32(const filepath[], const expectedCrc[]);
native FILE_GetCRC32(const filepath[], output[], outputSize);
native FILE_Compare(const path1[], const path2[]);
```

---

## Callbacks

### File Upload (Incoming)
```pawn
forward OnIncomingUploadCompleted(uploadId, routeId, const endpoint[], const filename[], 
                       const filepath[], const crc32[], crcMatched);
forward OnIncomingUploadFailed(uploadId, const reason[], const crc32[]);
forward OnIncomingUploadProgress(uploadId, percent);
```

### File Upload (Outgoing)
```pawn
forward OnOutgoingUploadStarted(uploadId);
forward OnOutgoingUploadProgress(uploadId, percent);
forward OnOutgoingUploadCompleted(uploadId, httpStatus, const responseBody[], const crc32[]);
forward OnOutgoingUploadFailed(uploadId, errorCode, const errorType[], const errorMessage[], httpStatus);
```

### Outbound Request / WebSocket
```pawn
forward OnRequestFailure(requestId, errorCode, const errorType[], const errorMessage[], httpStatus);
forward OnWebSocketDisconnect(socketId, bool:isJson, status, const reason[], reasonLen, errorCode);
```

---

## Built-in Endpoints

| Endpoint | Description |
|----------|-------------|
| `GET /health` | Server health check |
| `GET /stats` | Server statistics |
| `GET {route}/files` | List files (if allowed) |
| `GET {route}/files/{name}` | Download file (if allowed) |
| `GET {route}/files/{name}/info` | File metadata (if allowed) |
| `DELETE {route}/files/{name}` | Delete file (if allowed) |

---

## Client Examples

### JavaScript
```javascript
// GET players
const res = await fetch('http://localhost:8080/api/players', {
    headers: { 'Authorization': 'Bearer api-secret-key' }
});
const data = await res.json();

// POST ban
await fetch('http://localhost:8080/api/ban', {
    method: 'POST',
    headers: {
        'Authorization': 'Bearer api-secret-key',
        'Content-Type': 'application/json'
    },
    body: JSON.stringify({ playerId: 5, reason: 'cheating' })
});

// Upload file
const formData = new FormData();
formData.append('file', fileInput.files[0]);
await fetch('http://localhost:8080/maps', {
    method: 'POST',
    headers: { 'Authorization': 'Bearer upload-secret-key' },
    body: formData
});
```

### curl
```bash
# Health check
curl http://localhost:8080/health

# Get players
curl -H "Authorization: Bearer api-secret-key" http://localhost:8080/api/players

# Upload file
curl -X POST -H "Authorization: Bearer upload-secret-key" \
     -F "file=@mymap.map" http://localhost:8080/maps

# List files
curl -H "Authorization: Bearer upload-secret-key" http://localhost:8080/maps/files

# Download file
curl -H "Authorization: Bearer upload-secret-key" \
     http://localhost:8080/maps/files/mymap.map -o mymap.map
```

### File Route REST API Response Formats

**GET {route}/files** — List files
```json
{
  "success": true,
  "count": 3,
  "files": ["map1.map", "map2.json", "test.map"]
}
```

**GET {route}/files/{name}/info** — File info
```json
{
  "success": true,
  "name": "map1.map",
  "size": 12345,
  "modified": 1712419200
}
```

**DELETE {route}/files/{name}** — Delete file
```json
{
  "success": true,
  "deleted": "map1.map"
}
```

**GET {route}/files/{name}** — Download file  
Returns raw file content with appropriate `Content-Type` header.

---

## Complete Example

```pawn
#include <open.mp>
#include <PawnREST>

new g_MapRoute = -1;

public OnGameModeInit()
{
    REST_Start(8080);
    
    // File upload route
    g_MapRoute = FILE_RegisterRoute("/maps", "scriptfiles/maps/", ".map,.json", 50);
    FILE_AddAuthKey(g_MapRoute, "secret-key");
    FILE_AllowList(g_MapRoute, true);
    FILE_AllowDownload(g_MapRoute, true);
    FILE_AllowInfo(g_MapRoute, true);
    
    // Custom API routes
    REST_RegisterAPIRoute(HTTP_METHOD_GET, "/api/server", "API_GetServer");
    REST_RegisterAPIRoute(HTTP_METHOD_GET, "/api/players", "API_GetPlayers");
    REST_RegisterAPIRoute(HTTP_METHOD_POST, "/api/announce", "API_PostAnnounce");
    REST_RegisterAPIRoute(HTTP_METHOD_GET, "/api/player/{id}", "API_GetPlayer");
    
    return 1;
}

public API_GetServer(requestId)
{
    new payload = JsonObject();
    JsonSetString(payload, "name", "My Server");
    JsonSetInt(payload, "players", GetOnlineCount());
    JsonSetInt(payload, "maxPlayers", GetMaxPlayers());
    RespondNode(requestId, 200, payload);
    JsonCleanup(payload);
    return 1;
}

public API_GetPlayers(requestId)
{
    new page = REST_GetQueryInt(requestId, "page", 1);
    new limit = REST_GetQueryInt(requestId, "limit", 10);
    
    new payload = JsonObject();
    new players = JsonArray();
    JsonSetInt(payload, "page", page);
    
    for (new i = 0; i < MAX_PLAYERS; i++)
    {
        if (!IsPlayerConnected(i)) continue;
        
        new name[MAX_PLAYER_NAME];
        GetPlayerName(i, name, sizeof(name));
        
        new entry = JsonObject();
        JsonSetInt(entry, "id", i);
        JsonSetString(entry, "name", name);
        JsonSetInt(entry, "score", GetPlayerScore(i));
        JsonArrayAppend(players, entry);
        JsonCleanup(entry);
    }
    
    JsonSetObject(payload, "players", players);
    JsonCleanup(players);
    RespondNode(requestId, 200, payload);
    JsonCleanup(payload);
    return 1;
}

public API_PostAnnounce(requestId)
{
    new body = GetRequestJsonNode(requestId);
    if (body == -1)
    {
        RespondError(requestId, 400, "Invalid JSON body");
        return 1;
    }

    new message[256];
    JsonGetString(body, "message", message, sizeof(message));
    JsonCleanup(body);
    
    if (strlen(message) == 0)
    {
        RespondError(requestId, 400, "Message required");
        return 1;
    }
    
    SendClientMessageToAll(-1, message);
    
    new payload = JsonObject();
    JsonSetBool(payload, "success", true);
    JsonSetString(payload, "message", "Announcement sent");
    RespondNode(requestId, 200, payload);
    JsonCleanup(payload);
    return 1;
}

public API_GetPlayer(requestId)
{
    new playerId = REST_GetParamInt(requestId, "id");
    
    if (!IsPlayerConnected(playerId))
    {
        RespondError(requestId, 404, "Player not found");
        return 1;
    }
    
    new name[MAX_PLAYER_NAME];
    GetPlayerName(playerId, name, sizeof(name));
    
    new payload = JsonObject();
    JsonSetInt(payload, "id", playerId);
    JsonSetString(payload, "name", name);
    JsonSetInt(payload, "score", GetPlayerScore(playerId));
    JsonSetInt(payload, "ping", GetPlayerPing(playerId));
    RespondNode(requestId, 200, payload);
    JsonCleanup(payload);
    return 1;
}

public OnIncomingUploadCompleted(uploadId, routeId, const endpoint[], const filename[], 
                      const filepath[], const crc32[], crcMatched)
{
    printf("[PawnREST] Uploaded: %s -> %s (CRC: %s)", filename, filepath, crc32);
    return 1;
}

stock GetOnlineCount()
{
    new count = 0;
    for (new i = 0; i < MAX_PLAYERS; i++)
        if (IsPlayerConnected(i)) count++;
    return count;
}
```

---

## Credits

- **yhirose**: for the HTTP library
- **Southclaws**: for pawn-requests API refference
- **Implementation**: Fanorisky
- **SA-MP/open.mp Plugin SDK**: SA-MP Team, open.mp Contributors
