# Workflow & State Machine Rules

You are executing a multi-phase DAW software build. You must strictly control your task lifecycle by using the project's documentation as your state machine.

## 1. Task Initialization (The Map)
* **Rule:** At the start of ANY conversation, task, or prompt, you must immediately read `plan.md` to ground your understanding of the overarching engineering pipeline and locate your current Phase.
* **Execution:** Do not guess the architecture or implement adjacent features out of sequence. Stick strictly to the parameters outlined in the current active Phase of the plan.

## 2. Task Finalization (The Ledger)
* **Rule:** The moment a phase compiles cleanly via `./compileAndRun.sh` and meets the criteria, you must update `context.md`.
* **Execution:** 1. Open `context.md`.
  2. Locate the checkbox `[ ]` matching the phase you just completed and change it to `[x]`.
  3. Underneath that specific phase, add a concise bulleted list summarizing the exact changes, files created, and external libraries integrated.
  4. Prompt the user for approval to proceed to the next phase specified in `plan.md`.