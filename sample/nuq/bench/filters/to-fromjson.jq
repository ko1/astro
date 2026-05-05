[range(.) | tojson] | join(",") | "[" + . + "]" | fromjson | length
