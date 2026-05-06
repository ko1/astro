[.[] | select(.status >= 400) | .path] | unique | length
