# Cross-Platform Testing Skill Smoke Test

Use this procedure only when the user asks to test, smoke-test, or verify the
`cross-platform-testing` skill itself. This test checks subagent invocation and
response routing; it does not test Mac/Windows Git handoffs, native product
behavior, or physical devices.

## Procedure

1. Resolve and read `../agents/test-agent.toml` from this reference file's
   directory. Confirm its name is `test-agent`, its model is available, and its
   developer instruction is exactly:

   ```text
   When you are invoked, reply back to the agent that invoked you and say you are a test subagent that has successfully run.
   ```

2. Spawn exactly one fresh subagent using that definition. If the harness can
   invoke a skill-local TOML definition directly, select `test-agent`. Otherwise
   create a fresh subagent with the model and reasoning settings from the TOML
   and pass its exact developer instruction as the only task instruction.
3. Do not give the test subagent repository work, tools, files, Git operations,
   or additional goals. Wait only for its response.
4. The response must identify itself as a test subagent and state that it ran
   successfully. A canonical response is:

   ```text
   I am a test subagent that has successfully run.
   ```

   Accept equivalent grammar and contractions, such as `I'm a test subagent
   and have successfully run.` Do not accept a response that omits either the
   test-subagent identity or successful execution, changes the meaning, or
   reports that the subagent did not run.
5. Treat the test as failed if the definition is missing or invalid, the model
   is unavailable, spawning fails, the subagent times out, or its response does
   not match the expected response.

## Required final response

After the subagent responds or the attempt fails, end the main agent's turn
immediately with exactly one of these outcomes plus a concise failure reason
when applicable:

```text
Cross-platform-testing skill test succeeded: test-agent ran and replied correctly.
```

```text
Cross-platform-testing skill test failed: <concise reason>.
```

Do not begin the normal coordination workflow after reporting the result.
