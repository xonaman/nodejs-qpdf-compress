# Security Policy

## Supported Versions

Only the latest released version of `qpdf-compress` receives security updates.

| Version | Supported |
| ------- | --------- |
| latest  | ✅        |
| older   | ❌        |

## Reporting a Vulnerability

Please report security vulnerabilities **privately** through GitHub's
[private vulnerability reporting](https://github.com/xonaman/nodejs-qpdf-compress/security/advisories/new)
rather than opening a public issue.

`qpdf-compress` parses untrusted PDF input in native C/C++ code via QPDF,
mozjpeg, and HarfBuzz, so memory-safety issues are treated as high priority. We
aim to acknowledge reports within 7 days.

Please include:

- a description of the vulnerability and its impact,
- a minimal PDF or code sample that reproduces it,
- the affected package version and platform.
