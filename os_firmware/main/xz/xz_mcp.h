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

/**
 * @brief Initialize the PC MCP dual-channel bridge (USB Serial + Wi-Fi HTTP).
 */
void xz_mcp_init_pc_bridge(void);

/**
 * @brief Inspect a JSON line received over UART0 or USB-Serial/JTAG.
 * @return true if the line belonged to the PC MCP bridge (pc_mcp_resp / pc_hb / pc_hello).
 */
bool xz_mcp_on_serial_line(const char *line, bool from_usb_jtag);

/**
 * @brief Poll pending PC MCP request as a JSON string for HTTP channel (/api/pc_mcp/poll).
 *        Caller must free() the returned string.
 */
char *xz_mcp_poll_pending_http_json(void);

/**
 * @brief Submit a PC MCP response JSON received via HTTP (/api/pc_mcp/reply).
 */
bool xz_mcp_submit_http_resp_json(const char *json_str);

/**
 * @brief Check whether a Windows PC bridge client has sent a recent heartbeat or reply.
 */
bool xz_mcp_is_pc_connected(const char **out_channel);

#ifdef __cplusplus
}
#endif
