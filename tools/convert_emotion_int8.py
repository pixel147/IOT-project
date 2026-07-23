"""Convert ONNX emotion CNN to INT8 TFLite"""
import onnx
import numpy as np
import tensorflow as tf
from tensorflow import keras
import onnxruntime as ort

model_onnx = onnx.load('tools/emotion_cnn_aug.onnx')
W = {}
for init in model_onnx.graph.initializer:
    W[init.name] = onnx.numpy_helper.to_array(init)

def tf_w(w):
    return np.transpose(w, (2, 3, 1, 0)) if len(w.shape) == 4 else w

inp = keras.layers.Input((48, 48, 1))

def conv(ch, k, **kw):
    return keras.layers.Conv2D(ch, k, padding='same', **kw)

# Block 0: 1→8
l_sc0 = conv(8, 1, use_bias=False, name='b0_sc')
l_c1_0 = conv(8, 3, use_bias=True, name='b0_c1')
l_c2_0 = conv(8, 3, use_bias=True, name='b0_c2')
s0 = l_sc0(inp)
r0 = keras.layers.ReLU()(l_c1_0(inp))
a0 = keras.layers.Add()([l_c2_0(r0), s0])
x = keras.layers.MaxPool2D(2)(keras.layers.ReLU()(a0))

# Block 1: 8→16
l_sc2 = conv(16, 1, use_bias=False, name='b2_sc')
l_c1_2 = conv(16, 3, use_bias=True, name='b2_c1')
l_c2_2 = conv(16, 3, use_bias=True, name='b2_c2')
s2 = l_sc2(x)
r2 = keras.layers.ReLU()(l_c1_2(x))
a2 = keras.layers.Add()([l_c2_2(r2), s2])
x = keras.layers.MaxPool2D(2)(keras.layers.ReLU()(a2))

# Block 2: 16→32
l_sc4 = conv(32, 1, use_bias=False, name='b4_sc')
l_c1_4 = conv(32, 3, use_bias=True, name='b4_c1')
l_c2_4 = conv(32, 3, use_bias=True, name='b4_c2')
s4 = l_sc4(x)
r4 = keras.layers.ReLU()(l_c1_4(x))
a4 = keras.layers.Add()([l_c2_4(r4), s4])
x = keras.layers.MaxPool2D(2)(keras.layers.ReLU()(a4))

# Block 3: 32→64
l_sc6 = conv(64, 1, use_bias=False, name='b6_sc')
l_c1_6 = conv(64, 3, use_bias=True, name='b6_c1')
l_c2_6 = conv(64, 3, use_bias=True, name='b6_c2')
s6 = l_sc6(x)
r6 = keras.layers.ReLU()(l_c1_6(x))
a6 = keras.layers.Add()([l_c2_6(r6), s6])
x = keras.layers.MaxPool2D(2)(keras.layers.ReLU()(a6))

x = keras.layers.GlobalAveragePooling2D()(x)
x = keras.layers.Flatten()(x)
l_fc = keras.layers.Dense(7, use_bias=True, name='classifier')
out = l_fc(x)

model = keras.Model(inp, out)

# ── Load weights ──
pairs = [
    (l_sc0, [tf_w(W['features.0.shortcut.weight'])]),
    (l_c1_0, [tf_w(W['onnx::Conv_95']), W['onnx::Conv_96']]),
    (l_c2_0, [tf_w(W['onnx::Conv_98']), W['onnx::Conv_99']]),
    (l_sc2, [tf_w(W['features.2.shortcut.weight'])]),
    (l_c1_2, [tf_w(W['onnx::Conv_101']), W['onnx::Conv_102']]),
    (l_c2_2, [tf_w(W['onnx::Conv_104']), W['onnx::Conv_105']]),
    (l_sc4, [tf_w(W['features.4.shortcut.weight'])]),
    (l_c1_4, [tf_w(W['onnx::Conv_107']), W['onnx::Conv_108']]),
    (l_c2_4, [tf_w(W['onnx::Conv_110']), W['onnx::Conv_111']]),
    (l_sc6, [tf_w(W['features.6.shortcut.weight'])]),
    (l_c1_6, [tf_w(W['onnx::Conv_113']), W['onnx::Conv_114']]),
    (l_c2_6, [tf_w(W['onnx::Conv_116']), W['onnx::Conv_117']]),
    (l_fc, [np.transpose(W['classifier.3.weight']), W['classifier.3.bias']]),
]
for layer, wl in pairs:
    layer.set_weights(wl)

# ── Verify ──
sess = ort.InferenceSession('tools/emotion_cnn_aug.onnx')
x_test = np.random.randn(1, 1, 48, 48).astype(np.float32)
y_onnx = sess.run(None, {'input.1': x_test})[0]
y_tf = model.predict(x_test.transpose(0,2,3,1), verbose=0)
print(f'Max diff: {np.max(np.abs(y_onnx - y_tf)):.6f}')

# ── Convert to INT8 TFLite ──
def rep_data():
    for _ in range(200):
        yield [np.random.randint(0, 256, (1, 48, 48, 1)).astype(np.float32) / 255.0]

converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset = rep_data
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type = tf.uint8
converter.inference_output_type = tf.float32

tflite_model = converter.convert()
with open('tools/emotion_cnn_aug_int8.tflite', 'wb') as f:
    f.write(tflite_model)
print(f'\nINT8 saved: {len(tflite_model)} bytes ({len(tflite_model)/1024:.1f} KB)')

interpreter = tf.lite.Interpreter(model_content=tflite_model)
interpreter.allocate_tensors()
a = interpreter.get_input_details()[0]
b = interpreter.get_output_details()[0]
print(f'Input:  shape={a["shape"]} dtype={a["dtype"]}')
print(f'Output: shape={b["shape"]} dtype={b["dtype"]}')
