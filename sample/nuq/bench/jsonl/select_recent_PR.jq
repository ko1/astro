select(.type == "PullRequestEvent" and .payload.action == "opened") | {n: .payload.number, repo: .repo.name, user: .actor.login, title: .payload.pull_request.title}
