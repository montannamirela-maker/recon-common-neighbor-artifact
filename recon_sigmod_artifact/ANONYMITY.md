# Anonymity Checklist

Before uploading this repository for review, check the following:

- No author names, affiliations, personal emails, or advisor names appear in the repository.
- No absolute local paths such as `/Users/...` or home-directory paths appear in source code, scripts, README files, or logs.
- No private server names, usernames, access tokens, SSH hosts, API keys, or passwords are included.
- No large generated binary files are committed.
- Public datasets are referenced by source and preprocessing instructions rather than re-hosted under an identifying account.
- If a web landing page is used, it includes `noindex` metadata and the submission form uses the direct anonymous artifact URL, not a shortened or tracking link.

Suggested final check:

```bash
grep -RInE 'author|fudan|@|/Users|password|token|ssh|advisor|student' . \
  --exclude-dir=.git \
  --exclude='*.bin'
```

