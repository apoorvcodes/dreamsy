import type { NextConfig } from "next";

// The hosted MCP server (harness/src/mcp/http.ts) runs as its own process.
const MCP = process.env.DREAMSY_MCP_ORIGIN ?? "http://127.0.0.1:8090";

const config: NextConfig = {
  reactStrictMode: true,
  async headers() {
    return [{
      source: "/games/:file(.*\\.webp)",
      headers: [{ key: "Cache-Control", value: "public, max-age=86400" }],
    }];
  },
  async rewrites() {
    return [
      { source: "/mcp", destination: `${MCP}/mcp` },
      { source: "/mcp/:path*", destination: `${MCP}/mcp/:path*` },
      { source: "/.well-known/oauth-protected-resource/:path*", destination: `${MCP}/.well-known/oauth-protected-resource` },
      { source: "/.well-known/oauth-protected-resource", destination: `${MCP}/.well-known/oauth-protected-resource` },
      { source: "/.well-known/oauth-authorization-server/:path*", destination: `${MCP}/.well-known/oauth-authorization-server` },
      { source: "/.well-known/oauth-authorization-server", destination: `${MCP}/.well-known/oauth-authorization-server` },
      { source: "/.well-known/openid-configuration", destination: `${MCP}/.well-known/openid-configuration` },
    ];
  },
};

export default config;
