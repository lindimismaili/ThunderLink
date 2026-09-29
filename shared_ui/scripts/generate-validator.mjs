import Ajv2020 from 'ajv/dist/2020.js';
import standaloneCode from 'ajv/dist/standalone/index.js';
import { readFile, writeFile } from 'node:fs/promises';
const schema = JSON.parse(await readFile(new URL('../contract/messages.schema.json', import.meta.url), 'utf8'));
const ajv = new Ajv2020({ strict: true, code: { source: true, esm: true } });
const validate = ajv.compile(schema);
await writeFile(new URL('../src/generated-validator.js', import.meta.url), standaloneCode(ajv, validate));
