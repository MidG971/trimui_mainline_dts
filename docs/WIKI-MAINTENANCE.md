<!-- SPDX-License-Identifier: (GPL-2.0-only OR MIT) -->
<!-- Copyright (C) 2026 Midgy BALON -->

# Wiki maintenance and rendering checks

How the project wiki is structured, updated, and verified. The docs under `docs/`
stay the authoritative deep technical reference; the wiki pages summarise and link
into them.

## Where the wiki lives

- Separate repo: `git@github.com:MidG971/trimui_mainline_dts.wiki.git`, default
  branch **`master`**.
- Pages are `*.md` files at the repo root; `_Sidebar.md` is the navigation and
  `_Footer.md` the footer.
- Update flow: clone, edit, `git push origin master`. If a local working copy has
  lost its `.git` (it happened once), just re-clone rather than fighting it.

## Page names ↔ wiki links

GitHub resolves a `[[Wiki Link]]` to a page by replacing spaces with hyphens, so
`[[USB-C and DisplayPort]]` points at the file `USB-C-and-DisplayPort.md`. Keep the
filename and the link text in sync, and add new pages to `_Sidebar.md`.

## Verifying a page renders (do this before trusting a push)

Check the Markdown structure locally — these are the things that actually break
rendering on GitHub:

- **Wiki links resolve:** every `[[...]]` maps to an existing `*.md` page (slug =
  link text with spaces → hyphens).
- **Tables have a separator row:** a `| :--- | :--- |` line directly under the
  header. Without it, GitHub shows literal `|` pipes instead of a table.
- **Balanced markup:** backticks and brackets paired; fenced code blocks closed.

## Gotcha: WebFetch reports false "table not rendering" for GitHub wikis

Fetching a GitHub wiki URL through an HTML→Markdown fetch tool (e.g. WebFetch)
re-converts the already-rendered `<table>` back into a Markdown pipe-table, and
`<a>` links back into `[text](url)`. A summarising model can then misread that
re-conversion as "raw Markdown leaking" or "the table did not render."

**This is a false positive, not a page bug.** To confirm, fetch a *known-good,
unchanged* page (e.g. `Display-Bring-Up`) and ask the same question — if it shows
the identical "issue", the fault is the fetch tool, not your page.

Authoritative checks instead: the local structure lint above, or just open the
page in a browser.
