# Tailor Licensing

Copyright (c) 2026 ColdSun.

Tailor is distributed as `GPL-3.0-or-later` with the additional permissions in [EXCEPTIONS.md](EXCEPTIONS.md). This license covers Tailor's implementation and the compiled `Tailor.dll`. The complete GNU GPL version 3 text is in [LICENSE](LICENSE).

Tailor statically links CommonLibSSE-NG. CommonLibSSE-NG retains its own license and exceptions in the `lib/commonlibsse-ng` submodule.

## Permissive public interface

The distributable runtime-loading header `docs/api/TailorAPI.h` is separately available under the MIT License in `licenses/TailorAPI-MIT.txt`. It resolves optional exports from `Tailor.dll` at runtime and does not link against Tailor. This permission applies only to that public header and does not relicense Tailor's implementation, CommonLibSSE-NG, or dependencies that a consumer chooses to link.

Third-party components retain their respective licenses. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The preferred form for modifying Tailor is this repository with the CommonLibSSE-NG submodule initialized at the revision recorded by Git.

Binary release packages must include [LICENSE](LICENSE), [EXCEPTIONS.md](EXCEPTIONS.md), this file, [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and the applicable API and third-party license texts. Corresponding Source for the exact release, including its build files and pinned CommonLibSSE-NG revision, must accompany the binaries or be made available using a method permitted by GNU GPL version 3 section 6.
