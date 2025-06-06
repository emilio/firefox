const builtin = 'mix';
export const description = `
Validation tests for the ${builtin}() builtin.
`;

import { makeTestGroup } from '../../../../../../common/framework/test_group.js';
import { keysOf, objectsToRecord } from '../../../../../../common/util/data_tables.js';
import { kConvertableToFloatVectors, scalarTypeOf } from '../../../../../util/conversion.js';
import { ShaderValidationTest } from '../../../shader_validation_test.js';

import {
  ConstantOrOverrideValueChecker,
  fullRangeForType,
  kConstantAndOverrideStages,
  stageSupportsType,
  validateConstOrOverrideBuiltinEval,
} from './const_override_validation.js';

export const g = makeTestGroup(ShaderValidationTest);

const kValidArgumentTypes = objectsToRecord(kConvertableToFloatVectors);

g.test('values')
  .desc(
    `
Validates that constant evaluation and override evaluation of ${builtin}() never errors
`
  )
  .params(u =>
    u
      .combine('stage', kConstantAndOverrideStages)
      .combine('type', keysOf(kValidArgumentTypes))
      .filter(u => stageSupportsType(u.stage, kValidArgumentTypes[u.type]))
      .beginSubcases()
      .expand('a', u => fullRangeForType(kValidArgumentTypes[u.type], 5))
      .expand('b', u => fullRangeForType(kValidArgumentTypes[u.type], 5))
      .expand('c', u => fullRangeForType(kValidArgumentTypes[u.type], 5))
  )
  .fn(t => {
    const scalarType = scalarTypeOf(kValidArgumentTypes[t.params.type]);
    const vCheck = new ConstantOrOverrideValueChecker(t, scalarType);

    // Mix equation: a * (1 - c) + b * c
    // Should be invalid if the mix calculations result in intermediate
    // values that exceed the maximum representable float value for the given type.
    const a = Number(t.params.a);
    const b = Number(t.params.b);
    const c = Number(t.params.c);
    const c1 = vCheck.checkedResult(1 - c);
    const ac1 = vCheck.checkedResult(a * c1);
    const bc = vCheck.checkedResult(b * c);
    vCheck.checkedResult(ac1 + bc);

    const type = kValidArgumentTypes[t.params.type];

    // Validates mix(vecN(a), vecN(b), vecN(c));
    validateConstOrOverrideBuiltinEval(
      t,
      builtin,
      vCheck.allChecksPassed(),
      [type.create(t.params.a), type.create(t.params.b), type.create(t.params.c)],
      t.params.stage
    );

    // Validates mix(vecN(a), vecN(b), c));
    validateConstOrOverrideBuiltinEval(
      t,
      builtin,
      vCheck.allChecksPassed(),
      [type.create(t.params.a), type.create(t.params.b), scalarType.create(t.params.c)],
      t.params.stage
    );
  });

const kArgCases = {
  good: '(vec3(0), vec3(1), vec3(0.5))',
  bad_no_parens: '',
  // Bad number of args
  bad_0args: '()',
  bad_1arg: '(vec3(0))',
  bad_2arg: '(vec3(0), vec3(1))',
  bad_4arg: '(vec3(0), vec3(1), vec3(0.5), vec3(3))',
  // Bad value for arg 0
  bad_0bool: '(false, vec3(1), vec3(0.5))',
  bad_0array: '(array(1.1,2.2), vec3(1), vec3(0.5))',
  bad_0struct: '(modf(2.2), vec3(1), vec3(0.5))',
  bad_0int: '(1i, vec3(1), vec3(0.5))',
  bad_0uint: '(1u, vec3(1), vec3(0.5))',
  bad_0vec2i: '(vec2i(0), vec2(1), vec2(0.5))',
  bad_0vec3i: '(vec3i(0), vec3(1), vec3(0.5))',
  bad_0vec4i: '(vec4i(0), vec4(1), vec4(0.5))',
  bad_0vec2u: '(vec2u(0), vec2(1), vec2(0.5))',
  bad_0vec3u: '(vec3u(0), vec3(1), vec3(0.5))',
  bad_0vec4u: '(vec4u(0), vec4(1), vec4(0.5))',
  // Bad value type for arg 1
  bad_1bool: '(vec3(0), true, vec3(0.5))',
  bad_1array: '(vec3(0), array(1.1,2.2), vec3(0.5))',
  bad_1struct: '(vec3(0), modf(2.2), vec3(0.5))',
  bad_1int: '(vec3(0), 1i, vec3(0.5))',
  bad_1uint: '(vec3(0), 1u, vec3(0.5))',
  bad_1vec2i: '(vec2(1), vec2i(1), vec2(0.5))',
  bad_1vec3i: '(vec3(1), vec3i(1), vec3(0.5))',
  bad_1vec4i: '(vec4(1), vec4i(1), vec4(0.5))',
  bad_1vec2u: '(vec2(1), vec2u(1), vec2(0.5))',
  bad_1vec3u: '(vec3(1), vec3u(1), vec3(0.5))',
  bad_1vec4u: '(vec4(1), vec4u(1), vec4(0.5))',
  // Bad value type for arg 2
  bad_2bool: '(vec3(0), vec3(1), true)',
  bad_2array: '(vec3(0), vec3(1), array(1.1,2.2))',
  bad_2struct: '(vec3(0), vec3(1), modf(2.2))',
  bad_2int: '(vec3(0), vec3(1), 1i)',
  bad_2uint: '(vec3(0), vec3(1), 1u)',
  bad_2vec2i: '(vec2(1), vec2(1), vec2i(1))',
  bad_2vec3i: '(vec3(1), vec3(1), vec3i(1))',
  bad_2vec4i: '(vec4(1), vec4(1), vec4i(1))',
  bad_2vec2u: '(vec2(1), vec2(1), vec2u(1))',
  bad_2vec3u: '(vec3(1), vec3(1), vec3u(1))',
  bad_2vec4u: '(vec4(1), vec4(1), vec4u(1))',
};

g.test('args')
  .desc(`Test compilation failure of ${builtin} with variously shaped and typed arguments`)
  .params(u => u.combine('arg', keysOf(kArgCases)))
  .fn(t => {
    t.expectCompileResult(
      t.params.arg === 'good',
      `const c = ${builtin}${kArgCases[t.params.arg]};`
    );
  });

g.test('must_use')
  .desc(`Result of ${builtin} must be used`)
  .params(u => u.combine('use', [true, false]))
  .fn(t => {
    const use_it = t.params.use ? '_ = ' : '';
    t.expectCompileResult(t.params.use, `fn f() { ${use_it}${builtin}${kArgCases['good']}; }`);
  });
