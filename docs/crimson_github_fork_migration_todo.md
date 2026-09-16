# Crimson GitHub Fork Migration TODO

Date anchored: 2026-07-13.

Status: proposed; no GitHub repository migration has started.

## Objective

Represent Crimson on GitHub as an actual fork of the colleague-owned upstream
repository while preserving Crimson's complete Git history and keeping a safe,
reversible path back to the current standalone repository.

This migration should happen before GitHub Releases becomes the canonical
binary distribution channel. Git commits, branches, and tags can be pushed to a
new fork, but existing GitHub Releases and other repository-hosted metadata do
not move with Git objects.

## Current State

- Current GitHub remote:
  `git@github.com:jmdelahanty/crimson.git`
- Current local branch at the time this document was written:
  `codex/ui-monolith-20260404181029`
- The current checkout was created as a standalone GitHub repository rather
  than through GitHub's Fork action.
- The colleague-owned upstream repository URL has not yet been recorded.
- Upstream visibility and fork policy have not yet been verified.
- The local `gh` CLI API token is currently invalid. SSH Git operations and
  GitHub API authentication are separate; refresh `gh` authentication before
  attempting metadata inventory or repository creation.
- These commits were locally ahead of the recorded remote branch when this
  TODO was created:
  - `60f9457 ui: add user-scoped path preset editor`
  - `77f6857 decode: rewind after keyframe interval probe`
  - `47cc822 docs: record macOS stimulus encoding requirements`

Re-audit all of this at execution time. Do not assume the anchored state is
still current.

## GitHub Constraints That Control This Decision

- A GitHub fork is part of the upstream repository's network.
- All repositories in a fork network share the same visibility.
- A fork's visibility cannot be changed independently.
- A public upstream therefore produces a public fork.
- A private upstream produces a private fork and is subject to the upstream
  owner or organization's fork and permission policy.
- Organization rulesets can apply across a fork network and can prevent force
  pushes or other migration operations.
- GitHub provides a documented self-service path for detaching a fork, but no
  documented self-service setting for converting an arbitrary standalone
  repository in place into somebody else's fork.
- Creating a new real fork and pushing Git refs into it is therefore the
  supported migration model unless GitHub Support explicitly offers a safer
  metadata-preserving attachment.

Official references:

- [About forks](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/working-with-forks/about-forks)
- [Fork permissions and visibility](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/working-with-forks/about-permissions-and-visibility-of-forks)
- [Creating a fork](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/working-with-forks/fork-a-repo)
- [Duplicating a repository](https://docs.github.com/en/repositories/creating-and-managing-repositories/duplicating-a-repository)
- [Detaching a fork](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/working-with-forks/detaching-a-fork)

## Decision Gates

Do not create or modify a GitHub fork until every gate is answered.

- [ ] Record the colleague-owned upstream repository URL.
- [ ] Record the upstream default branch name.
- [ ] Confirm whether the upstream is public, private, or internal.
- [ ] Confirm that Crimson is permitted to use that same visibility.
- [ ] Confirm that upstream licensing permits the Crimson modifications and
      intended binary distribution.
- [ ] For a private/internal upstream, confirm that its owner permits forking
      into the intended personal account or organization.
- [ ] Confirm who will own the destination fork:
  - [ ] `jmdelahanty`
  - [ ] an HHMI organization
  - [ ] another approved organization
- [ ] Confirm who must be able to download future Crimson Releases.
- [ ] Confirm whether those users will have read access to the resulting fork.
- [ ] Decide whether to ask GitHub Support if the existing repository can be
      attached to the upstream fork network while retaining GitHub metadata.
- [ ] Decide what happens to the current repository's issues, pull requests,
      Actions history, settings, and any future releases.

### Visibility Stop Condition

If the colleague's upstream is public but Crimson must remain private, stop the
fork migration. Keep Crimson standalone and configure the colleague repository
as an `upstream` Git remote instead. A private fork of a public GitHub
repository is not available.

In that case, document the derivation and license prominently in Crimson's
README, preserve the upstream remote, and periodically merge approved upstream
changes without claiming GitHub fork-network membership.

## Non-Negotiable Preservation Rules

- Do not delete the current GitHub repository during migration.
- Do not force-push any current or candidate default branch until the ancestry
  and ref inventory has been reviewed.
- Do not use `git push --mirror` against the candidate fork without inspecting
  every ref it would create, update, or delete.
- Do not change repository visibility as a migration shortcut.
- Do not publish a public fork until a secret and sensitive-content audit has
  passed.
- Preserve every reachable current commit, branch, and tag in at least two
  independently verified locations before cutover.
- Keep the old GitHub repository available as an archive until the candidate
  fork has passed build, runtime, ref, and metadata validation.
- Do not begin canonical GitHub Release publication until the repository
  identity and ownership decision is final.

## What Git Pushes Preserve

The migration can preserve:

- commit objects and parent relationships;
- commit authorship and timestamps;
- file history;
- local and remote branch tips;
- annotated and lightweight tags; and
- every file tree represented by those refs.

## What Git Pushes Do Not Preserve

The migration does not automatically carry over:

- issues;
- pull requests, reviews, or comments;
- Actions workflow run history and artifacts;
- GitHub Releases and release assets;
- repository secrets and environments;
- branch protection settings and rulesets;
- collaborators and teams;
- stars, watchers, or traffic history;
- repository webhooks, deploy keys, or GitHub Apps;
- wiki content unless separately cloned and migrated; or
- GitHub Projects and Discussions.

The current standalone repository should initially remain the authoritative
archive for that metadata.

## Phase 0: Authentication and Change Freeze

- [ ] Choose a low-activity migration window.
- [ ] Ask contributors not to push during the final ref inventory and cutover.
- [ ] Refresh GitHub CLI authentication:

  ```bash
  gh auth login -h github.com -p ssh
  gh auth status
  ```

- [ ] Verify SSH access independently:

  ```bash
  ssh -T git@github.com
  ```

- [ ] Confirm the worktree is clean or that every intentional local change is
      committed.
- [ ] Push the current active branch to the existing standalone repository.
- [ ] Record the current repository owner, visibility, default branch, and
      branch protection state.

## Phase 1: Upstream and Ancestry Audit

Use a temporary remote name so the current `origin` is not disturbed.

```bash
UPSTREAM_URL=git@github.com:COLLEAGUE/REPOSITORY.git
git remote add audit-upstream "$UPSTREAM_URL"
git fetch audit-upstream --prune --tags
git remote show audit-upstream
```

- [ ] Record the exact upstream URL.
- [ ] Record the upstream default branch.
- [ ] Record the upstream HEAD commit.
- [ ] Test whether the current Crimson history shares an ancestor with the
      upstream default branch:

  ```bash
  git merge-base HEAD audit-upstream/main
  ```

- [ ] Save the merge-base commit if one exists.
- [ ] Inspect the relationship:

  ```bash
  git log --graph --decorate --oneline --boundary \
      audit-upstream/main...HEAD
  git rev-list --left-right --count audit-upstream/main...HEAD
  ```

- [ ] Determine which case applies:
  - [ ] Crimson is a normal descendant of an upstream commit.
  - [ ] Crimson and current upstream share ancestry but both have diverged.
  - [ ] Crimson's Git history is disconnected from upstream.
- [ ] Identify the likely original upstream base commit from source history,
      documentation, tags, or contributor records.

Do not synthesize a history connection merely to make GitHub display a clean
ahead/behind count. Any history-bridging commit must truthfully document what
was done.

## Phase 2: Complete Ref Inventory

Record local branches and tags:

```bash
git for-each-ref --sort=refname \
  --format='%(refname) %(objectname)' refs/heads refs/tags \
  > crimson-local-refs-before-fork.txt
```

Record the existing GitHub repository refs:

```bash
git ls-remote --heads --tags origin \
  > crimson-origin-refs-before-fork.txt
```

- [ ] Inventory every local branch.
- [ ] Inventory every branch present only on the existing remote.
- [ ] Inventory all annotated and lightweight tags.
- [ ] Identify branches with names that will conflict with upstream branches.
- [ ] Identify tags with the same name but different object IDs.
- [ ] Record Git LFS use, if any:

  ```bash
  git lfs ls-files
  ```

- [ ] Record submodule URLs and commits:

  ```bash
  git submodule status --recursive
  ```

- [ ] Review whether temporary agent, experiment, or abandoned branches need
      permanent GitHub preservation or only bundle preservation.

## Phase 3: Independent Backups

Create a portable bundle containing all currently reachable refs:

```bash
git bundle create crimson-before-fork-migration.bundle --all
git bundle verify crimson-before-fork-migration.bundle
```

- [ ] Store the verified bundle somewhere outside this checkout.
- [ ] Create a second copy on a different filesystem or approved storage
      location.
- [ ] Test-clone the bundle into a temporary directory.
- [ ] Compare its branch and tag object IDs with the Phase 2 inventory.
- [ ] Push all new Crimson commits to the current standalone GitHub repository.
- [ ] Confirm the existing GitHub repository contains every intended branch and
      tag before creating the candidate fork.

### GitHub Metadata Inventory

- [ ] Export or record open and closed issues.
- [ ] Export or record open and closed pull requests.
- [ ] Record Releases and release assets.
- [ ] Record Actions workflows, secrets by name, environments, and protection
      rules. Never export secret values into the repository.
- [ ] Record branch protections and rulesets.
- [ ] Record collaborators, teams, deploy keys, webhooks, and installed apps.
- [ ] Record repository topics, description, homepage, and default branch.

Example read-only inventory commands:

```bash
gh repo view jmdelahanty/crimson \
  --json nameWithOwner,visibility,defaultBranchRef,url
gh issue list --repo jmdelahanty/crimson --state all --limit 1000
gh pr list --repo jmdelahanty/crimson --state all --limit 1000
gh release list --repo jmdelahanty/crimson --limit 1000
```

## Phase 4: Create a Non-Destructive Candidate Fork

Keep `jmdelahanty/crimson` untouched during candidate creation.

- [ ] Create a real fork using a temporary distinguishable name such as:

  ```text
  crimson-fork-migration
  ```

- [ ] Select the intended owner.
- [ ] Do not select "Copy the DEFAULT branch only" if upstream branch
      preservation is required.
- [ ] Verify GitHub displays `forked from OWNER/UPSTREAM`.
- [ ] Verify the fork has the expected visibility.
- [ ] Verify the upstream owner and organization permissions are acceptable.
- [ ] Add the candidate as a new remote without changing `origin`:

  ```bash
  git remote add candidate \
      git@github.com:DESTINATION/crimson-fork-migration.git
  git fetch candidate --prune --tags
  ```

- [ ] Record the candidate's initial branch and tag object IDs.

## Phase 5: Populate the Candidate Without Destructive Overwrites

Do not begin with a blind mirror push. The fork already contains upstream refs,
and conflicting names must be reconciled deliberately.

- [ ] Push the current product tip to an unambiguous candidate branch:

  ```bash
  git push candidate HEAD:refs/heads/crimson-main
  ```

- [ ] Push non-conflicting Crimson branches under their intended names.
- [ ] For conflicting branch names, initially use a namespace such as
      `crimson-migration/BRANCH_NAME`.
- [ ] Push non-conflicting tags.
- [ ] Resolve conflicting tags individually; do not replace published tags
      silently.
- [ ] If Git LFS is used, push the required LFS objects explicitly.
- [ ] Produce a candidate ref inventory and compare it to the bundle and old
      repository inventories.

## Phase 6: Reconcile Repository History

### Case A: Current Crimson Is a Descendant of Upstream

This is the preferred and simplest case.

- [ ] Confirm the proposed default-branch update is a fast-forward or an
      intentional normal merge.
- [ ] Push the Crimson product branch.
- [ ] Set the candidate default branch to the Crimson product branch.
- [ ] Retain a clean upstream-tracking branch or remote configuration.

### Case B: Histories Share an Ancestor but Have Diverged

- [ ] Review upstream changes since the merge base.
- [ ] Merge the appropriate upstream branch into a migration branch.
- [ ] Resolve conflicts without dropping Crimson behavior.
- [ ] Build and test the merge result before making it the default branch.
- [ ] Preserve the pre-merge Crimson tip with a named branch or tag.

### Case C: Histories Are Unrelated

Choose one approach explicitly:

- [ ] Reconstruct the true upstream base and replay/import Crimson history when
      practical. This is preferred when the original derivation can be proven.
- [ ] Keep the histories as separate branches and accept that GitHub comparison
      and upstream pull requests will be limited.
- [ ] Create one documented two-parent bridge merge that retains Crimson's
      current tree while connecting the two histories.

An example bridge operation is shown below, but it must not be executed without
review because it changes future merge semantics:

```bash
git switch -c migration/connect-upstream CURRENT_CRIMSON_TIP
git merge --allow-unrelated-histories -s ours audit-upstream/main \
  -m "chore: document and reconnect Crimson upstream ancestry"
```

- [ ] Explain the migration and the `ours` merge strategy in the commit body.
- [ ] Confirm both complete histories are reachable from the bridge commit.
- [ ] Confirm the bridge commit's file tree exactly matches the approved Crimson
      tree.
- [ ] Test a future upstream merge on a disposable branch before accepting the
      bridge design.

## Phase 7: Candidate Validation

### Ref and Object Validation

- [ ] Run `git fsck --full` against a fresh clone of the candidate.
- [ ] Compare all required branch tip object IDs.
- [ ] Compare all required tag object IDs.
- [ ] Confirm every commit reachable from the old repository is reachable from
      either the candidate fork or the verified archive bundle.
- [ ] Confirm the candidate default branch tree matches the approved current
      Crimson tree:

  ```bash
  git diff --exit-code CURRENT_CRIMSON_TIP candidate/crimson-main
  ```

- [ ] Confirm GitHub displays the correct upstream fork relationship.
- [ ] Confirm expected upstream comparisons and pull-request bases work.

### Security and Visibility Validation

- [ ] Confirm the candidate's visibility before pushing any sensitive branch.
- [ ] Run secret scanning appropriate to the intended visibility.
- [ ] Confirm no datasets, credentials, private paths, or prohibited binaries
      were accidentally committed.
- [ ] Review the upstream license and Crimson notices.
- [ ] Confirm private-fork access inheritance with the upstream owner or
      organization.

### Build and Runtime Validation

- [ ] Build the normal Linux target from a fresh candidate clone.
- [ ] Run focused unit and CTest coverage.
- [ ] Run the authenticated GPU GUI playback smoke.
- [ ] Build and validate the staged Linux app drop.
- [ ] Repeat the supported Windows build and runtime checks when a Windows
      builder is available.
- [ ] Repeat Apple Silicon build and feasibility checks when the macOS port is
      ready.

## Phase 8: GitHub Metadata and Policy Setup

Configure the candidate before it becomes primary.

- [ ] Set the approved default branch.
- [ ] Recreate branch protection or repository rulesets.
- [ ] Recreate Actions permissions and environments.
- [ ] Recreate secret names through approved secret-management procedures.
- [ ] Recreate collaborators and team access.
- [ ] Recreate required webhooks and GitHub App installations.
- [ ] Add repository description, topics, homepage, license, and attribution.
- [ ] Configure vulnerability reporting and `SECURITY.md` if applicable.
- [ ] Confirm future Release publishers have `contents: write` only where
      required.
- [ ] Confirm fork-network rules do not prevent the intended release workflow.

## Phase 9: Cutover

Perform cutover only after every candidate validation passes.

- [ ] Announce the cutover window.
- [ ] Freeze pushes to the old repository.
- [ ] Repeat the ref inventory and push any final commits.
- [ ] Rename the old repository to an archival name such as:

  ```text
  crimson-standalone-archive
  ```

- [ ] Mark the archived repository read-only in its description and README.
- [ ] Rename the validated fork to `crimson` if retaining the current repository
      name is desired.
- [ ] Do not depend on GitHub's rename redirect after reusing the old name.
- [ ] Update local remotes explicitly:

  ```bash
  git remote rename origin archive
  git remote rename candidate origin
  git remote rename audit-upstream upstream
  git remote -v
  ```

- [ ] Update other developer clones and automation remotes.
- [ ] Update documentation, badges, clone commands, submodule references, and
      deployment configuration.
- [ ] Push and fetch a disposable test branch to verify permissions.
- [ ] Confirm the archived repository and bundle remain accessible.

## Phase 10: Post-Cutover Observation

- [ ] Keep the old repository intact for an agreed observation period.
- [ ] Monitor clone, fetch, push, PR, and Actions behavior.
- [ ] Confirm contributors are using the new remote.
- [ ] Confirm upstream fetch and comparison workflows behave as intended.
- [ ] Confirm GitHub Release access matches the intended audience.
- [ ] Do not delete the archive solely because the Git migration passed; retain
      it as long as its GitHub-only metadata is needed.

## Rollback Plan

Rollback must remain possible without rewriting developer clones.

- [ ] Stop pushes to the candidate fork.
- [ ] Repoint `origin` to the archived standalone repository.
- [ ] Restore the pre-migration default branch and protections there.
- [ ] Verify all refs against `crimson-before-fork-migration.bundle`.
- [ ] Communicate that the candidate fork is not authoritative.
- [ ] Preserve the failed candidate for diagnosis; do not delete evidence until
      the cause is understood.

Example local rollback:

```bash
git remote set-url origin \
  git@github.com:jmdelahanty/crimson-standalone-archive.git
git fetch origin --prune --tags
```

## Release Deployment Dependency

The GitHub Release deployment work should resume only after this migration
reaches a stable outcome.

- [ ] Final repository owner and URL are known.
- [ ] Release audience has appropriate repository access.
- [ ] Release workflows and secrets are configured on the final repository,
      not the temporary candidate.
- [ ] Release tags are created only in the authoritative repository.
- [ ] No production Release asset exists solely in the archived standalone
      repository unless intentionally retained for historical access.

## Definition of Done

- [ ] GitHub identifies the new Crimson repository as a fork of the correct
      colleague-owned upstream repository.
- [ ] The final visibility and access model has explicit approval.
- [ ] Every required current commit, branch, and tag is preserved and verified.
- [ ] The candidate default branch contains the complete approved Crimson tree.
- [ ] Upstream ancestry is either naturally preserved or explicitly and
      truthfully reconciled.
- [ ] A fresh clone builds and passes Crimson's required tests and playback
      smoke.
- [ ] GitHub settings required for protected development and Releases are
      recreated.
- [ ] The old standalone repository and verified Git bundle provide a tested
      rollback path.
- [ ] All developers and automation use the final fork remote.
- [ ] The GitHub Release deployment TODO can proceed against the final
      repository identity.

## Execution Record

Fill this section during the migration.

| Field | Value |
| --- | --- |
| Upstream URL | pending |
| Upstream owner | pending |
| Upstream visibility | pending |
| Upstream default branch | pending |
| Upstream HEAD at audit | pending |
| Crimson pre-migration HEAD | pending |
| Shared merge base | pending |
| Existing repository archive URL | pending |
| Candidate fork URL | pending |
| Final Crimson URL | pending |
| Verified bundle path and checksum | pending |
| Migration operator | pending |
| Cutover time | pending |
| Validation result | pending |
| Rollback observation end date | pending |
