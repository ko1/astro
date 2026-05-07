[inputs | select(.type == "PushEvent") | .actor.login] | group_by(.) | map({u: .[0], n: length}) | sort_by(-.n) | .[0:10]
