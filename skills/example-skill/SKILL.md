---
name: example-skill
description: Counts lines, words, and bytes in a text file using the bundled count.sh script. Use when asked to count/measure a file's size in these terms.
---
# Example skill

Run the bundled script instead of doing this by hand:

```
sh <skill-directory>/count.sh <path-to-file>
```

`<skill-directory>` is the "Skill directory" path given above this text.
The script prints three numbers - lines, words, bytes - in that order, one
per line. Report all three back to the user.

If `count.sh` is missing or the target file doesn't exist, say so instead
of guessing the counts.
