# Fuchey Threat Model & Security Policy

## Core Directives
1. **Private Keys**: Never exposed in logs, memory dumps, or network requests. Zeroed out on lock/reset.
2. **AI Boundary**: AI can request transactions but CANNOT sign directly.
3. **Physical Confirmation**: Transactions exceeding the spending threshold MUST require a physical button press.
