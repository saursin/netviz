# Vendored assets

Small binary assets checked into the repository and embedded into the
executable at build time (see `cmake/EmbedFile.cmake`). Code dependencies are
not vendored; they are fetched at pinned commits by `cmake/Dependencies.cmake`.

| Directory         | What                        | Version | Source                                                             | License    |
|-------------------|-----------------------------|---------|--------------------------------------------------------------------|------------|
| `lucide/`         | Lucide icon font            | 1.49.0  | npm `lucide-static` (`font/lucide.ttf`, `font/codepoints.json`)    | ISC        |
| `inter/`          | Inter, Regular (UI font)    | 4.1     | github.com/rsms/inter release `Inter-4.1.zip` (`extras/ttf/`)      | OFL-1.1    |
| `jetbrains-mono/` | JetBrains Mono, Regular     | 2.304   | github.com/JetBrains/JetBrainsMono release `JetBrainsMono-2.304.zip` | OFL-1.1  |

Archive checksums (sha256) at the time of import:

- `lucide-static-1.49.0.tgz`: npm integrity `sha512-gSjtl7bR2v26oDInh2Wua1PrF2BRRXl9inhnLWcFfnRqRShmgZvlFRisPHqTfU7kxsuZU8ThNjAFotbInrzKow==`
- `Inter-4.1.zip`: `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`
- `JetBrainsMono-2.304.zip`: `6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf`

To add a Lucide icon, look up its codepoint in `lucide/codepoints.json` and add
a UTF-8 constant to `src/netviz/gui/fonts.h`.
