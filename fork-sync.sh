#!/bin/bash
#
# fork-sync.sh — helpers for maintaining the Randalix/vkdt fork
#
# Remotes:
#   origin   = git@github.com:Randalix/vkdt.git  (our fork)
#   upstream = https://github.com/hanatos/vkdt.git (original)
#
set -euo pipefail

UPSTREAM_REMOTE="upstream"
FORK_REMOTE="origin"
MAIN_BRANCH="master"

usage() {
    cat <<'EOF'
Usage: ./fork-sync.sh <command>

Commands:
  status      Show how far ahead/behind the fork is vs upstream
  sync        Fast-forward master to upstream/master and push to fork
  rebase      Rebase current feature branch onto latest upstream/master
  log         Show commits in current branch not in upstream/master
  diff        Show full diff of current branch vs upstream/master
  push        Push current branch to the fork remote
  pr          Open a GitHub PR from current branch to fork's master
  upstream-pr Open a GitHub PR from fork to upstream (contribute back)
EOF
}

cmd_status() {
    git fetch "$UPSTREAM_REMOTE" --quiet
    git fetch "$FORK_REMOTE" --quiet 2>/dev/null || true

    local branch
    branch=$(git rev-parse --abbrev-ref HEAD)

    echo "Current branch: $branch"
    echo ""

    # ahead/behind upstream
    local counts
    counts=$(git rev-list --left-right --count "$UPSTREAM_REMOTE/$MAIN_BRANCH"..."$branch" 2>/dev/null || echo "0 0")
    local behind ahead
    behind=$(echo "$counts" | awk '{print $1}')
    ahead=$(echo "$counts" | awk '{print $2}')

    echo "vs upstream/$MAIN_BRANCH:"
    echo "  $ahead commit(s) ahead"
    echo "  $behind commit(s) behind"

    # ahead/behind fork
    if git rev-parse "$FORK_REMOTE/$MAIN_BRANCH" &>/dev/null; then
        counts=$(git rev-list --left-right --count "$FORK_REMOTE/$MAIN_BRANCH"..."$branch" 2>/dev/null || echo "0 0")
        behind=$(echo "$counts" | awk '{print $1}')
        ahead=$(echo "$counts" | awk '{print $2}')
        echo ""
        echo "vs origin/$MAIN_BRANCH:"
        echo "  $ahead commit(s) ahead"
        echo "  $behind commit(s) behind"
    fi
}

cmd_sync() {
    echo "Fetching upstream..."
    git fetch "$UPSTREAM_REMOTE"

    local branch
    branch=$(git rev-parse --abbrev-ref HEAD)

    if [ "$branch" != "$MAIN_BRANCH" ]; then
        echo "Not on $MAIN_BRANCH — switching..."
        git checkout "$MAIN_BRANCH"
    fi

    echo "Fast-forwarding $MAIN_BRANCH to $UPSTREAM_REMOTE/$MAIN_BRANCH..."
    if ! git merge --ff-only "$UPSTREAM_REMOTE/$MAIN_BRANCH"; then
        echo ""
        echo "ERROR: Cannot fast-forward. Your $MAIN_BRANCH has diverged from upstream."
        echo "You may need to reset or rebase. Aborting."
        exit 1
    fi

    echo "Pushing $MAIN_BRANCH to $FORK_REMOTE..."
    git push "$FORK_REMOTE" "$MAIN_BRANCH"

    echo ""
    echo "Done. Fork's $MAIN_BRANCH is now up to date with upstream."
}

cmd_rebase() {
    local branch
    branch=$(git rev-parse --abbrev-ref HEAD)

    if [ "$branch" = "$MAIN_BRANCH" ]; then
        echo "Already on $MAIN_BRANCH — use 'sync' instead."
        exit 1
    fi

    echo "Fetching upstream..."
    git fetch "$UPSTREAM_REMOTE"

    echo "Rebasing $branch onto $UPSTREAM_REMOTE/$MAIN_BRANCH..."
    git rebase "$UPSTREAM_REMOTE/$MAIN_BRANCH"

    echo ""
    echo "Done. Run './fork-sync.sh push' to update the fork remote."
}

cmd_log() {
    git fetch "$UPSTREAM_REMOTE" --quiet
    echo "Commits in current branch not in upstream/$MAIN_BRANCH:"
    echo ""
    git log --oneline "$UPSTREAM_REMOTE/$MAIN_BRANCH"..HEAD
}

cmd_diff() {
    git fetch "$UPSTREAM_REMOTE" --quiet
    git diff "$UPSTREAM_REMOTE/$MAIN_BRANCH"...HEAD
}

cmd_push() {
    local branch
    branch=$(git rev-parse --abbrev-ref HEAD)
    git push -u "$FORK_REMOTE" "$branch"
}

cmd_pr() {
    local branch
    branch=$(git rev-parse --abbrev-ref HEAD)

    if [ "$branch" = "$MAIN_BRANCH" ]; then
        echo "Create a feature branch first."
        exit 1
    fi

    git push -u "$FORK_REMOTE" "$branch"
    gh pr create --repo Randalix/vkdt --base "$MAIN_BRANCH" --head "$branch"
}

cmd_upstream_pr() {
    local branch
    branch=$(git rev-parse --abbrev-ref HEAD)

    if [ "$branch" = "$MAIN_BRANCH" ]; then
        echo "Create a feature branch first."
        exit 1
    fi

    git push -u "$FORK_REMOTE" "$branch"
    gh pr create --repo hanatos/vkdt --base "$MAIN_BRANCH" --head "Randalix:$branch"
}

case "${1:-}" in
    status)      cmd_status ;;
    sync)        cmd_sync ;;
    rebase)      cmd_rebase ;;
    log)         cmd_log ;;
    diff)        cmd_diff ;;
    push)        cmd_push ;;
    pr)          cmd_pr ;;
    upstream-pr) cmd_upstream_pr ;;
    -h|--help|"") usage ;;
    *)
        echo "Unknown command: $1"
        usage
        exit 1
        ;;
esac
