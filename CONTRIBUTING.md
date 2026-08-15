Contributing
============

Master copy of this project is hosted on GitHub:
https://github.com/glikely/obs-ptz

Anyone may contribute to this project.
Contributions are licensed under GPLv2 and must be made with a Developer
Certificate of Origin [DCO] "Signed-off-by:" attestation as described below,
indicating that you wrote the code and have the right to pass it on as an open
source patch under the GPLv2 license.
Patches that are not signed off will not be accepted.

Use GitHub pull requests to submit proposed changes.

Also, please write good git commit messages. A good commit message looks like this:

```
Header line: explain the commit in one line (use the imperative)

Body of commit message is a few lines of text, explaining things
in more detail, possibly giving some background about the issue
being fixed, etc etc.

The body of the commit message can be several paragraphs, and
please do proper word-wrap and keep columns shorter than about
74 characters or so. That way "git log" will show things
nicely even when it's indented.

Make sure you explain your solution and why you're doing what you're
doing, as opposed to describing what you're doing. Reviewers and your
future self can read the patch, but might not understand why a
particular solution was implemented.

Reported-by: whoever-reported-it
Assisted-by: AGENT_NAME:MODEL_VERSION
Signed-off-by: Your Name <you@example.com>
```

Where that header line really should be meaningful, and really should be just
one line. That header line is what is shown by tools like gitk and shortlog,
and should summarize the change in one readable line of text, independently of
the longer explanation. Please use verbs in the imperative in the commit
message, as in "Fix bug that...", "Add file/feature ...", or "Make plugin ..."

AI-Assisted Contributions
--------------------------

AI tools (LLM-based coding assistants, agents, etc.) are welcome as an aid
when preparing a contribution. If one was used to help write the code or
find/fix a bug, acknowledge it with an `Assisted-by:` tag, next to
`Signed-off-by:`, following the same format adopted by the Linux kernel
([Documentation/process/coding-assistants.rst]):

```
Assisted-by: AGENT_NAME:MODEL_VERSION
```

- `AGENT_NAME:MODEL_VERSION` - the AI tool or framework and the specific
  model version it used, colon-separated, e.g. `Claude:claude-sonnet-5`.

For example:

```
Assisted-by: Claude:claude-sonnet-5
```

This doesn't change anything else about how a contribution is judged or
accepted: your `Signed-off-by:` still certifies you have the right to
submit the change under the DCO below, and you're still responsible for
reviewing, understanding, and standing behind everything in the patch,
AI-assisted or not. `Assisted-by:` is a record of how the patch was
produced, not a transfer of that responsibility - and per the kernel's
same policy, AI agents themselves must never add a `Signed-off-by:`; only
a human can certify the DCO.

[Documentation/process/coding-assistants.rst]: https://docs.kernel.org/process/coding-assistants.html

DCO Attestation
---------------

To help track the origin of contributions, this project uses the same
[DCO] "sign-off" process as used by the Linux kernel.
The sign-off is a simple line at the end of the explanation for the
patch, which certifies that you wrote it or otherwise have the right to
pass it on as an open-source patch.
The rules are pretty simple: if you can certify the below:

### Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

        (a) The contribution was created in whole or in part by me and I
            have the right to submit it under the open source license
            indicated in the file; or

        (b) The contribution is based upon previous work that, to the best
            of my knowledge, is covered under an appropriate open source
            license and I have the right under that license to submit that
            work with modifications, whether created in whole or in part
            by me, under the same open source license (unless I am
            permitted to submit under a different license), as indicated
            in the file; or

        (c) The contribution was provided directly to me by some other
            person who certified (a), (b) or (c) and I have not modified
            it.

        (d) I understand and agree that this project and the contribution
            are public and that a record of the contribution (including all
            personal information I submit with it, including my sign-off) is
            maintained indefinitely and may be redistributed consistent with
            this project or the open source license(s) involved.

then you just add a line saying::

        Signed-off-by: Random J Developer <random@developer.example.org>

[DCO]: https://developercertificate.org/
