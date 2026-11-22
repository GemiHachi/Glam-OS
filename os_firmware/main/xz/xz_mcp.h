/**
 * @file xz_mcp.h
 * @brief Device-side MCP (Model Context Protocol, JSON-RPC 2.0) Handler for XiaoZhi AI
 */

#pragma once

#include "cJSON.h"
#include "xz_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Process an incoming MCP JSON-RPC 2.0 payload from the XiaoZhi server.
 *
 * @param session_id Current XiaoZhi WebSocket session_id (may be empty)
 * @param payload    Parsed cJSON object of the "payload" field
 * @param hooks      OS control callbacks
 * @return Newly malloc'd JSON string to send back over WebSocket (caller must free),
 *         or NULL if no response is required (e.g. notification).
 */
char *xz_mcp_handle_payload(const char *session_id, const cJSON *payload, const xz_os_hooks_t *hooks);

#ifdef __cplusplus
}
#endif
