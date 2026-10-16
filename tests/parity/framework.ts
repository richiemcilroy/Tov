// An app on a JavaScript framework (node_modules/parity-framework, shaped like velox), whose
// handlers are this file's code. tests/parity.rs serves it with `bun` and its Tov port
// (framework.tov.ts) natively, and compares the responses: a handler's exception, a bad input
// rejected by the framework's validation included, is the framework's to answer (issue #20).
import { createApp } from "parity-framework"

class Refused extends Error {
  constructor(message: string) {
    super(message)
    this.name = "Refused"
  }
}

function user(ctx: any): any {
  const p = ctx.params
  return `hello ${p.name}`
}

function readName(ctx: any): any {
  return ctx.params.name
}

function helper(ctx: any): any {
  const name = readName(ctx)
  return `helper ${name}`
}

function guarded(ctx: any): any {
  try {
    return `guarded ${ctx.params.name}`
  } catch (e) {
    return `guarded caught ${e.name}: ${e.message}`
  }
}

const app = createApp()
app.route("/users/:name", user)
app.route("/helper/:name", helper)
app.route("/guarded/:name", guarded)
app.route("/async/:name", async (ctx: any) => {
  await Promise.resolve(0)
  return `async ${ctx.params.name}`
})
app.route("/fail", (ctx: any) => ctx.fail("bang"))
app.route("/refused", (ctx: any): any => {
  throw new Refused("not today")
})
app.route("/query", (ctx: any) => `query ${ctx.query.q}`)
app.listen(Number(process.env.PORT ?? 3000))
