/*
 * ipc_client.h - Higher-level IPC client library
 * Copyright (c) 2026 OpSys Project
 *
 * Provides a simplified interface for service discovery
 * and request/response messaging.
 */

#ifndef LIBIPC_IPC_CLIENT_H
#define LIBIPC_IPC_CLIENT_H

/**
 * Connect to a named service port.
 * Looks up the port by name via the port registry.
 * @param service_name  Name of the service to connect to.
 * @return Port handle (>= 0), or negative error code.
 */
int ipc_connect(const char *service_name);

/**
 * Send a request and wait for a response (synchronous call).
 * @param port     Target port handle.
 * @param req      Request data.
 * @param req_len  Request length in bytes.
 * @param resp     Response buffer (caller-allocated).
 * @param resp_len In: buffer capacity, Out: actual response size.
 * @return 0 on success, negative error code on failure.
 */
int ipc_request(int port, const void *req, int req_len, void *resp, int resp_len);

#endif /* LIBIPC_IPC_CLIENT_H */
