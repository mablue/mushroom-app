# AGENTS.md — AI Agent Instructions

> Read this file **before making any changes** to this repository.

This repository is a **fixed-stack template** used for AI-driven website generation.

---

## Strict rules — do not violate

1. **Tech stack is fixed.** Do not add, remove, or swap any framework or library. The stack (React, TypeScript, Vite, Tailwind CSS, SST) is locked and must not change.
2. **No database in this phase.** Do not add any database driver, ORM, or DB connection code (e.g. Prisma, Drizzle, pg, mysql2, mongoose, SQLite). A database (RDS) will be provided in a future phase.
3. **Frontend pages only.** Only create or modify files under `src/`. Do not add backend Lambda handlers, API routes, or server-side logic.
4. **The entire application must be stateless.** This repo runs on AWS Lambda (serverless). No in-memory state, no file-system writes, no session storage on the server side. All state must live in the browser or in future external services.
5. **Run commands are fixed.** Do not modify `package.json` scripts. The commands listed in [README.md](./README.md#commands) are the only ones permitted and must not be changed.

---

## Template rules

These constraints are non-negotiable:

- Keep phase 1 static and frontend-only.
- **Do not add DB packages** (no Prisma, Drizzle, pg, mysql2, mongoose, or any DB client).
- **Do not add backend Lambda handlers** or server-side code.
- **Do not modify run commands** in `package.json`.
- **Do not introduce stateful server-side logic** — the app must remain fully stateless.
- Keep API integration behind `src/lib/api.ts`.
- Keep copy/content in `src/data/site.ts` when possible.
- A future phase will introduce RDS; wait for that phase before adding any data-persistence logic.

---

## Project structure

```txt
src/
  components/     reusable UI components
  data/           editable site content
  lib/            config, API helpers, utilities
  pages/          page-level React views
```

---

For build commands, environment variables, and architecture details see [README.md](./README.md).
