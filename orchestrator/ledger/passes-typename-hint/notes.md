# Notes (passes-typename-hint, 2026-10-10)
- typeName "TextPass[]" is the contract with the frontend PassListEditor (frontend PR #25). Keep the string in sync. The specialization is in engine/include/engine/text/TextJson.hpp. The TextRenderer member key is _effectPasses, UIText is effectPasses.
