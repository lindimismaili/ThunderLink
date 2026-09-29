import Ajv2020 from 'ajv/dist/2020.js';
import { readFile, readdir } from 'node:fs/promises';
const schema = JSON.parse(await readFile(new URL('../contract/messages.schema.json', import.meta.url), 'utf8'));
const validate = new Ajv2020({ strict: true }).compile(schema);
const dir = new URL('../contract/fixtures/', import.meta.url);
for (const name of await readdir(dir)) {
  if (!name.endsWith('.json')) continue;
  const value = JSON.parse(await readFile(new URL(name, dir), 'utf8'));
  const actual = validate(value);
  if (actual !== name.startsWith('valid-')) throw new Error(`Contract fixture ${name} did not match expectation: ${JSON.stringify(validate.errors)}`);
}
console.log('Contract fixtures validated.');
