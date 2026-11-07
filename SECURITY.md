# Security — Wokwi CI Token

## What happened
You pasted `WOKWI_CLI_TOKEN=wok_ppJy...ade0d97ed` in plaintext chat on 2026-09-07. That token is now compromised via chat history / logs.

## Immediate fix (do now, 30s)
1. https://wokwi.com/dashboard/tokens → find token ending `ade0d97ed` → **Delete**
2. **Create new token** → copy once → store in password manager
3. Local: `echo 'WOKWI_CLI_TOKEN=wok_NEWSecret' > hardware/.env`  (or `export WOKWI_CLI_TOKEN=wok_NEWSecret` per shell)
4. GitHub: Repo → Settings → Secrets and variables → Actions → **New repository secret** `WOKWI_CLI_TOKEN` = new value (used by `.github/workflows/wokwi-ci.yml`)
5. Never paste tokens in chat/issues again — use `.env` (gitignored) + GitHub Secrets.

## How this repo handles secrets (correct)
- `.env` is in `.gitignore` — never committed
- `.env.example` shows placeholder only
- `run-cli.sh` reads `$WOKWI_CLI_TOKEN` from env, never prints value (only length)
- GitHub Actions reads `secrets.WOKWI_CLI_TOKEN` — not hardcoded

## To verify new token works (without VS Code)
```bash
export WOKWI_CLI_TOKEN=wok_NEWSecret
npx @wokwi/cli --version
bash hardware/run-cli.sh   # should print [✓] WOKWI_CLI_TOKEN present (XX chars)
```
