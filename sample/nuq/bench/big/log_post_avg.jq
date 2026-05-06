[.[] | select(.method == "POST") | .duration_ms] | if length > 0 then (add / length) else null end
