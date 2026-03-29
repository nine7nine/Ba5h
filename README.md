# Ba5h

A fork of **Bash 5.3** that integrates PowerShell-inspired interactive features directly into the shell's C core. Ba5h brings modern line-editing UX -- inline suggestions, live syntax highlighting, predictive text, and frecency-based directory jumping -- without requiring external plugins, Python runtimes, or framework scripts.

## Features

### Inline Suggestions (PowerShell / Fish Style)

Readline-level suggestion engine that renders ghost text ahead of the cursor, sourced from command history via prefix and substring matching.

| Keybinding | Action |
|---|---|
| Right Arrow | Accept full suggestion |
| Ctrl+Right | Accept next word (partial accept) |
| Down Arrow | Dismiss (with fade animation) |
| Alt+Up / Alt+Down | Cycle through suggestions |

### Live Syntax Highlighting

A single-pass state-machine tokenizer colorizes the input line in real-time as you type. Highlighted elements include:

- Keywords and builtins
- Strings and variables
- Comments and operators
- Redirections and errors

Uses bright/high-intensity ANSI codes and 256-color palette, tuned for dark terminals. Ghost/suggestion text is left in plain suggestion color so it remains visually distinct from typed input.

### Pluggable Prediction Framework

A PowerShell `PSReadLine`-inspired predictor registry. Predictors are priority-ordered; the first non-NULL result wins.

| Priority | Predictor | Description |
|---|---|---|
| 10 | **history** | History-based prefix/substring suggestions |
| 15 | **frecency** | Directory suggestions for `cd` commands |
| 20 | **command** | Builtin, alias, and function name completion |
| 25 | **completion** | COMPSPEC `-W` wordlist matching |

### Completion-Aware Predictor

Uses programmable completion specs (COMPSPEC) to suggest arguments for known commands. Fast-paths only: literal `-W` wordlists and in-memory `-A` action flags (builtins, aliases, functions). Intentionally skips `-F` shell functions and `-C` external commands to keep suggestions instant.

Subcommand suggestions for common CLI tools can be registered shell-side via `complete -W` in `~/.bash_predict_completions`.

### Frecency-Based Directory Prediction

The frecency predictor suggests directories for `cd` commands scored by:

```
frequency / (1 + hours_since_last_visit / 4)
```

State is persisted to `~/.bash_frecency` and hooks into `cd` via `bindpwd()`. Supports cycling through matches with Up/Down arrows, with matched text highlighted within the suggestion.

## Configuration

### Enabling Features (`~/.inputrc`)

Ba5h's interactive features are controlled via Readline variables. Add the following to your `~/.inputrc`:

```
set enable-inline-suggestions on
set suggestion-strategy substring
set enable-syntax-highlighting on
```

| Variable | Values | Description |
|---|---|---|
| `enable-inline-suggestions` | `on` / `off` | Enables ghost-text suggestions from history and predictors |
| `suggestion-strategy` | `substring` / `prefix` | Match strategy for history suggestions |
| `enable-syntax-highlighting` | `on` / `off` | Enables real-time syntax colorization of the input line |

### Completion Wordlists (`~/.bash_predict_completions`)

To enable subcommand suggestions for CLI tools, define wordlists in `~/.bash_predict_completions` using `complete -W`:

```bash
# ~/.bash_predict_completions
complete -W "install remove update upgrade search show list" apt
complete -W "push pull commit checkout branch merge rebase log status diff stash clone fetch reset" git
complete -W "build run test fmt vet mod get install" go
```

Then source the file in your `~/.bashrc`:

```bash
# ~/.bashrc
source ~/.bash_predict_completions
```

The completion-aware predictor will use these wordlists to offer instant argument suggestions as you type.

## Building

Ba5h uses the standard Bash autoconf build system.

```bash
./configure --prefix=/usr/local
make -j$(nproc)
sudo make install
```

This installs the shell to `/usr/local/bin/bash`.

To use it as your default login shell:

```bash
echo '/usr/local/bin/bash' | sudo tee -a /etc/shells
chsh -s /usr/local/bin/bash
```

## Base Version

Forked from **GNU Bash 5.3-release**.

## License

Ba5h inherits the [GNU General Public License v3](https://www.gnu.org/licenses/gpl-3.0.html) from upstream Bash.
