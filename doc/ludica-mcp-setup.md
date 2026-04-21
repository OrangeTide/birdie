# setup ludica-mcp

Step 1: build it

Step 2: add to your mcp settings

in .claude/settings.json
```json
{
  "mcpServers": {
    "ludica": {
      "command": "_out/x86_64-linux-gnu/bin/ludica-mcp-bridge",
      "env": { "LUDICA_MCP_PORT": "4000" }
    }
  }
}
```

Step 3: run the launcher before starting claude

LUDICA_MCP_ALLOWEXEC=$(echo _out/*/bin/* | tr ' ' ':') _out/x86_64-linux-gnu/bin/ludica-launcher &
