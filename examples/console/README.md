# Console examples

Small, runnable Zym scripts that show off `Console` from `docs/console.md`.

Run any of them with:

```
zym examples/console/<name>.zym
```

| Script | What it shows |
| --- | --- |
| `colors.zym` | The 16 named colors, bright variants, and a 24-bit truecolor gradient |
| `styles.zym` | Bold / dim / italic / underline / reverse / strikethrough |
| `progress_bar.zym` | A classic `[####----]` progress bar that updates in place |
| `spinner.zym` | A braille spinner with elapsed time |
| `typewriter.zym` | Character-by-character "typing" effect |
| `rainbow.zym` | Animated rainbow text using truecolor |
| `bouncing_ball.zym` | A ball bouncing across the alternate screen |
| `matrix.zym` | A short Matrix-style falling green columns demo |
| `dashboard.zym` | A tiny live "dashboard" with a header, bars, and a clock |

All animated examples either run for a fixed duration or stop on their own; nothing requires you to press a key. Examples that take over the screen (`bouncing_ball`, `matrix`, `dashboard`) use the alternate screen and restore it on exit.

If you ever exit one of these abnormally and your terminal looks broken, run `reset` from your shell.
