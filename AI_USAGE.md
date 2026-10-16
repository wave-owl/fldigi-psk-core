# KI-Nutzung — Token-Ledger

<!-- ai-tokens-ledger v1 — automatisch gepflegt von ki-skills/ai-tokens; nicht von Hand bearbeiten -->

Eine Zeile pro Commit mit KI-Unterstützung (Copilot CLI, Claude Code, Codex CLI).
Die Zeile wird beim Commit (`pre-commit`-Hook) im **selben** Commit ergänzt; die
Zahlen entsprechen den `AI-Tokens:`-Trailern der Commit-Nachricht. Anonym: nur
Tool, Modell und Zahlen. Abschalten pro Repo: `git config ai-tokens.ledger false`.

| Spalte | Bedeutung |
|---|---|
| ID | Zeilenkennung (8 Hex = beim Commit erfasst, `bf-<sha>` = per `backfill` aus Trailern). Commit finden: `git log -S'| <ID> |' --reverse -- AI_USAGE.md` |
| Datum | lokale Zeit des Commits (ISO 8601) |
| Betreff | erste Zeile der Commit-Nachricht; `(offen)` = beim Commit unbekannt, wird später nachgetragen |
| Agent/Modell(e) | `tool/modell` |
| Calls | Modellaufrufe |
| Input | Prompt-Tokens inkl. Cache |
| Cached | davon Cache-Lesezugriffe |
| Output | Output-Tokens inkl. Reasoning |
| AIU | Copilot-Kosteneinheiten (nur Copilot) |
| Zeilen c/t/d/k/− | hinzugefügte/geänderte Zeilen Code/Tests/Doku/Konfig, gelöschte Zeilen |
| Std. ohne KI | geschätzte Personenstunden ohne KI (Heuristik, siehe unten) |

**Aufwandsschätzung — grobe Heuristik, keine Messung:**

    h = 1 × (0.25 + Code/15 + Tests/30 + Doku/60 + Konfig/60 + Gelöscht/300)

Raten in Zeilen pro Stunde, Fixaufwand je Commit in Stunden. Grundlage: 10–50
ausgelieferte LOC je Personentag über den ganzen Lebenszyklus großer Projekte
(McConnell, Jones); für fokussierte Einzelarbeit sind ~15 LOC/h Code inkl.
Entwurf, Debugging und Selbstreview eine optimistische Größenordnung. Tests sind
repetitiver, Doku/Specs/Konfig ≈ 600 Wörter/h, Löschen/Umbau schnell. Nicht
gezählt: Binärdateien, Lockfiles, generierte/vendorte Dateien, `AI_USAGE.md`.
Anpassbar: `git config ai-tokens.effort-factor`, `ai-tokens.rate-{code,tests,docs,config,deleted}`,
`ai-tokens.effort-overhead`, `ai-tokens.effort-exclude`.

## Summe

| Commits | Calls | Input | Cached | Output | AIU | Std. ohne KI |
|--:|--:|--:|--:|--:|--:|--:|
| 1 | 312 | 37674012 | 36339199 | 247083 | 1860.4 | 91.0 |

## Commits

| ID | Datum | Betreff | Agent/Modell(e) | Calls | Input | Cached | Output | AIU | Zeilen c/t/d/k/− | Std. ohne KI |
|---|---|---|---|--:|--:|--:|--:|--:|---|--:|
| 1156d22a | 2026-10-16T23:37+02:00 | Native BPSK receiver for PSK31/63/125 after fldigi 4.2.13 | copilot/claude-opus-5.5 | 312 | 37674012 | 36339199 | 247083 | 1860.4 | 549/1199/778/75/0 | 91.0 |
