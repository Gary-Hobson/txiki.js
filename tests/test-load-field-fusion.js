import assert from 'tjs:assert';

function readArgument(object) {
    return object.value;
}

for (let i = 0; i < 100; i++) {
    const object = { value: i };

    object.self = object;
    assert.eq(readArgument(object), i, 'own field of argument');
    assert.eq(object.self, object, 'self-referential field');

    let local = {
        get value() {
            local = { value: 456 };

            return 789;
        }
    };

    assert.eq(local.value, 789, 'getter return value');
    assert.eq(local.value, 456, 'getter can replace local receiver');

    function mutateArgument(argument) {
        Object.defineProperty(argument, 'value', {
            get() {
                argument = { value: 77 };

                return 88;
            }
        });
        const result = argument.value;

        assert.eq(result, 88, 'getter on argument returns its value');
        assert.eq(argument.value, 77, 'getter can replace argument receiver');
    }

    mutateArgument({});

    const proxy = new Proxy(object, {
        get(target, key) {
            return key === 'value' ? 42 : Reflect.get(target, key);
        }
    });

    assert.eq(readArgument(proxy), 42, 'proxy get trap');
    assert.throws(() => readArgument({
        get value() {
            throw new Error('getter failed');
        }
    }), Error, 'getter exception');
}
