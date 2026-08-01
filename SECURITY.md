# Security

Report vulnerabilities privately to the repository maintainers. Do not include
tokens, device logs, personal data, or unpublished project content in a public
issue.

DeviceExplorer is a development tool. The host dashboard binds to localhost,
device connections use a session token, file access is restricted to registered
roots, and the runtime client is disabled in Shipping builds. Keep those
properties intact when extending the protocol.
