[inputs | .type] | group_by(.) | map({t: .[0], n: length}) | sort_by(-.n)
