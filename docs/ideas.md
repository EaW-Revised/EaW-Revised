# Ideas and feature requests

The project collects new features and wishes on the **Ideas board**, a category of GitHub
Discussions: [github.com/EaW-Revised/EaW-Revised/discussions/categories/ideas](https://github.com/EaW-Revised/EaW-Revised/discussions/categories/ideas).
Anyone with a GitHub account can post an idea and upvote the ones they want most. Upvotes
are how the community tells maintainers what to look at first.

## Where things go

| You have | Post it as |
|---|---|
| A new feature, an improvement or a wish | An idea on the [Ideas board](https://github.com/EaW-Revised/EaW-Revised/discussions/categories/ideas) |
| A question about building, running or modding | A [Q&A discussion](https://github.com/EaW-Revised/EaW-Revised/discussions/categories/q-a) |
| Something that is broken, crashes or lags | A [bug report](https://github.com/EaW-Revised/EaW-Revised/issues/new?template=bug_report.yml) |
| A place where EaW Revised behaves differently from the original game | A [behaviour difference](https://github.com/EaW-Revised/EaW-Revised/issues/new?template=feature_request.yml) issue |
| A security problem | A private report, see [SECURITY.md](../SECURITY.md) |

Issues track work that is concrete or agreed. Ideas are where a feature is discussed and
weighed before that. If you are unsure, post an idea: a maintainer can move it.

## Posting an idea

1. **Search first.** Open the board [sorted by most upvotes](https://github.com/EaW-Revised/EaW-Revised/discussions/categories/ideas?discussions_q=category%3AIdeas+sort%3Atop)
   and search for your idea. If it is there, upvote it and add your use case as a
   comment. One post with forty votes is clearer than four posts with ten.
2. **One idea per post.** Separate ideas can be planned, voted on and closed separately.
3. **Fill in the form.** Say what you want from a player's or mod author's point of view,
   and why it matters. A concrete example ("in a 20-ship battle I cannot tell which frigates
   are damaged") helps more than a solution alone.
4. **Keep it clean-room.** Describe the original game in your own words: what you saw
   while playing, or what its XML data says. Never paste or attach game files, screenshots
   or recordings of the original game, decompiler output, binary addresses or original
   source code ([clean-room rules](../CONTRIBUTING.md#the-clean-room-rules)). Posts that
   do are edited or removed.

## Voting

- Use the **upvote arrow** on the post. Upvotes are the count maintainers sort by;
  emoji reactions and "+1" comments are not counted.
- Comment when you have something to add: a use case, a mod that needs it, an edge case,
  or how the original game behaves.
- Upvote as many ideas as you like. You can take a vote back at any time.

## What happens to an idea

Maintainers go through the board regularly, starting with the most upvoted ideas, and
always before planning a new phase or a release ([roadmap](../plan/backlog.md)). Each
reviewed idea gets one status label:

| Label | Meaning |
|---|---|
| `idea` | New, not reviewed yet (set by the form). |
| `idea: planned` | Accepted. A linked issue tracks the work, or the roadmap names it. |
| `idea: needs observation` | Wanted, but how the original game behaves must first be observed and written up as a [behaviour note](behaviour/README.md). Help with observations is welcome. |
| `idea: later` | Reasonable, but depends on parts that do not exist yet (for example land battles, galactic conquest or multiplayer). It stays open and keeps collecting votes. |
| `idea: shipped` | Implemented. The post is closed as resolved with a link to the release. |
| `idea: declined` | Not planned. The post is closed with the reason. |

Votes are an important input, not a ballot. Maintainers also weigh the
[roadmap](../plan/backlog.md) and its phase order, the effort, and whether an idea fits the
project's rules: the simulation must stay deterministic, original behaviour must come from
clean-room observation, and mods that work with the original game must keep working.

Ideas are declined when they would need game files or original code in the repository,
break existing mods or determinism, or fall outside the project's goals. A declined idea
can be raised again when circumstances change; link the old post.

## Turning an idea into work

When an idea is planned, a maintainer creates an issue from the discussion ("Create issue
from discussion" on the post), links it, and labels the idea `idea: planned`. Want to
build it yourself? Say so on the idea first so the approach can be agreed, then follow
[CONTRIBUTING.md](../CONTRIBUTING.md).
