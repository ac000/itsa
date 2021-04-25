# Contributing

When sending code, please either send signed-off patches or a pull request
with signed-off commits. This means adding a line that says
"Signed-off-by: Name \<Email\>" at the end of each commit. E.g

```
Signed-off-by: Andrew Clayton <andrew@digital-domain.net>
```

This signifies that you have read/understood/and agreed to the
[Developer's Certificate of Origin](DCO). Essentially indicating that you
wrote the code and/or have the right to contribute it to this project.

This is **not** a CLA.

Also, please write good git commit messages. A good commit message looks like
this:

```
Header line: explaining the commit in one line

Body of commit message is a few lines of text, explaining things in
more detail, possibly giving some background about the issue being
fixed, etc etc.

The body of the commit message can be several paragraphs, and please do
proper word-wrap and keep columns less than or equal to 72 characters.
That way "git log" will show things nicely even when it's indented.

Reported-by: whoever-reported-it
Signed-off-by: Your Name <you@example.com>
```

That header line really should be meaningful, and really should be just one
line. The header line is what is shown by tools like gitk and shortlog, and
should summarize the change in one readable line of text, independently of
the longer explanation.

- If emailing patches, it is recommended to use git-send-email(1).
- If emailing a pull request it is recommended to use git-request-pull(1).
- Pull requests can be made via GitHub.

Email should be sent to the project maintainer;

```
Andrew Clayton <andrew@digital-domain.net>
```
