// Copyright © 2023-2024 Apple Inc.
#include <cassert>
#include <numeric>

#include "mlx/fast.h"
#include "mlx/fast_primitives.h"
#include "mlx/ops.h"
#include "mlx/transforms.h"

namespace mlx::core::fast {

std::vector<array> Custom::vjp(
    const std::vector<array>& primals,
    const std::vector<array>& cotangents,
    const std::vector<int>& argnums,
    const std::vector<array>& outputs) {
  auto [_, vjps] = mlx::core::vjp(fallback_, primals, cotangents);
  std::vector<array> vjp_outs;
  for (int i = 0, j = 0; i < vjps.size(); ++i) {
    if (j < argnums.size() && i == argnums[j]) {
      vjp_outs.push_back(vjps[i]);
      j++;
    }
  }
  return vjp_outs;
}

std::vector<array> Custom::jvp(
    const std::vector<array>& primals,
    const std::vector<array>& tangents,
    const std::vector<int>& argnums) {
  std::vector<array> all_tangents;
  for (int i = 0, j = 0; i < primals.size(); i++) {
    if (j < argnums.size() && i == argnums[j]) {
      all_tangents.emplace_back(tangents[j++]);
    } else {
      all_tangents.emplace_back(zeros_like(primals[i]));
    }
  }
  auto [_, jvps] = mlx::core::jvp(fallback_, primals, all_tangents);
  return jvps;
}

std::pair<std::vector<array>, std::vector<int>> Custom::vmap(
    const std::vector<array>& inputs,
    const std::vector<int>& axes) {
  auto outputs = mlx::core::vmap(fallback_, axes)(inputs);
  auto out_axes = std::vector<int>(outputs.size(), 0);
  return {outputs, out_axes};
}

array rms_norm(
    const array& x,
    const std::optional<array>& weight,
    float eps,
    StreamOrDevice s_ /* = {} */) {
  bool has_weight = weight.has_value();

  if (x.ndim() == 0) {
    std::ostringstream msg;
    msg << "[rms_norm] Input must have at least 1 dimension but got input with "
           "0 dimensions.";
    throw std::invalid_argument(msg.str());
  }
  if (has_weight) {
    if ((*weight).ndim() != 1) {
      std::ostringstream msg;
      msg << "[rms_norm] (*weight) must have 1 dimension but has "
          << (*weight).ndim() << " dimensions.";
      throw std::invalid_argument(msg.str());
    }
    if ((*weight).size() != x.shape(-1)) {
      std::ostringstream msg;
      msg << "[rms_norm] (*weight) must have the same size as the last dimension of"
             " x but has "
          << (*weight).size() << " elements.";
      throw std::invalid_argument(msg.str());
    }
  }

  auto out_type = (weight.has_value()) ? result_type(x, (*weight)) : x.dtype();
  if (!issubdtype(out_type, floating)) {
    std::ostringstream msg;
    msg << "[rms_norm] Received unsupported type " << out_type << ".";
    throw std::invalid_argument(msg.str());
  }

  auto s = to_stream(s_);
  auto fallback =
      [has_weight, eps, out_type, s](const std::vector<array>& inputs) {
        auto x = astype(inputs[0], float32, s);
        x = multiply(
            x,
            rsqrt(
                add(mean(square(x, s), -1, /* keepdims */ true, s),
                    array(eps, float32),
                    s),
                s),
            s);
        x = astype(x, out_type, s);

        if (has_weight) {
          x = multiply(x, inputs[1], s);
        }

        return std::vector<array>{x};
      };

  auto passed_weight =
      (has_weight) ? astype(*weight, out_type, s) : array(1, out_type);

  if (!RMSNorm::use_fallback(s)) {
    return array(
        x.shape(),
        out_type,
        std::make_shared<RMSNorm>(s, fallback, eps),
        {astype(x, out_type, s), passed_weight});
  }
  return fallback({x, passed_weight})[0];
}

std::vector<array> RMSNorm::vjp(
    const std::vector<array>& primals,
    const std::vector<array>& cotangents,
    const std::vector<int>& argnums,
    const std::vector<array>& outputs) {
  assert(primals.size() == 2);
  assert(outputs.size() == 1);
  assert(cotangents.size() == 1);

  auto s = stream();
  auto fallback = [eps = eps_, s](const std::vector<array>& inputs) {
    auto& x = inputs[0];
    auto& w = inputs[1];
    auto& g = inputs[2];

    std::vector<array> vjps;

    auto n = rsqrt(
        add(mean(square(x, s), /* axis= */ -1, /* keepdims= */ true, s),
            array(eps, x.dtype()),
            s),
        s);
    auto n3 = power(n, array(3, x.dtype()), s);

    // df/dx
    auto gw = multiply(g, w, s);
    auto t = mean(multiply(gw, x, s), /* axis= */ -1, /* keepdims= */ true, s);
    t = multiply(multiply(x, t, s), n3, s);
    vjps.push_back(subtract(multiply(gw, n, s), t, s));

    // df/dw
    std::vector<int> axes(g.ndim() - 1);
    std::iota(axes.begin(), axes.end(), 0);
    if (w.ndim() == 0) {
      vjps.push_back(zeros_like(w, s));
    } else {
      vjps.push_back(sum(
          multiply(g, multiply(x, n, s), s), axes, /* keepdims= */ false, s));
    }

    return vjps;
  };

  auto vjps = array::make_arrays(
      {primals[0].shape(), primals[1].shape()},
      {primals[0].dtype(), primals[1].dtype()},
      std::make_shared<RMSNormVJP>(s, fallback, eps_),
      {primals[0], primals[1], cotangents[0]});

  std::vector<array> returned_vjps;
  for (auto& arg : argnums) {
    returned_vjps.push_back(std::move(vjps[arg]));
  }

  return returned_vjps;
}

bool RMSNorm::is_equivalent(const Primitive& other) const {
  const RMSNorm& a_other = static_cast<const RMSNorm&>(other);
  return eps_ == a_other.eps_;
}

bool RMSNormVJP::is_equivalent(const Primitive& other) const {
  const RMSNormVJP& a_other = static_cast<const RMSNormVJP&>(other);
  return eps_ == a_other.eps_;
}

array layer_norm(
    const array& x,
    const std::optional<array>& weight,
    const std::optional<array>& bias,
    float eps,
    StreamOrDevice s_ /* = {} */) {
  bool has_weight = weight.has_value();
  bool has_bias = bias.has_value();

  if (x.ndim() == 0) {
    std::ostringstream msg;
    msg << "[layer_norm] Input must have at least 1 dimension but got input with "
           "0 dimensions.";
    throw std::invalid_argument(msg.str());
  }
  if (has_weight && (*weight).ndim() != 1) {
    std::ostringstream msg;
    msg << "[layer_norm] weight must have 1 dimension but has "
        << (*weight).ndim() << " dimensions.";
    throw std::invalid_argument(msg.str());
  }
  if (has_bias && (*bias).ndim() != 1) {
    std::ostringstream msg;
    msg << "[layer_norm] bias must have 1 dimension but has " << (*bias).ndim()
        << " dimensions.";
    throw std::invalid_argument(msg.str());
  }

  auto out_type = (has_weight)
      ? ((has_bias) ? result_type(x, *weight, *bias) : result_type(x, *weight))
      : x.dtype();
  if (!issubdtype(out_type, floating)) {
    std::ostringstream msg;
    msg << "[layer_norm] Received unsupported type " << out_type << ".";
    throw std::invalid_argument(msg.str());
  }

  auto s = to_stream(s_);
  auto fallback = [has_weight, has_bias, eps, out_type, s](
                      const std::vector<array>& inputs) {
    auto x = astype(inputs[0], float32, s);

    auto mu = mean(x, /* axis= */ -1, /* keepdims= */ true, s);
    auto xc = subtract(x, mu, s);
    auto v = mean(square(xc, s), /* axis= */ -1, /* keepdims= */ true, s);

    x = multiply(xc, rsqrt(add(v, array(eps, float32), s), s));
    x = astype(x, out_type, s);

    // If the LN is affine then transform x according to the weight and bias
    if (has_weight) {
      x = multiply(x, inputs[1], s);
    }
    if (has_bias) {
      x = add(x, inputs[2], s);
    }

    return std::vector<array>{x};
  };

  auto passed_weight =
      (has_weight) ? astype(*weight, out_type, s) : array(1, out_type);
  auto passed_bias =
      (has_bias) ? astype(*bias, out_type, s) : array(0, out_type);

  if (!LayerNorm::use_fallback(s)) {
    return array(
        x.shape(),
        out_type,
        std::make_shared<LayerNorm>(s, fallback, eps),
        {astype(x, out_type, s), passed_weight, passed_bias});
  }
  return fallback({x, passed_weight, passed_bias})[0];
}

std::vector<array> LayerNorm::vjp(
    const std::vector<array>& primals,
    const std::vector<array>& cotangents,
    const std::vector<int>& argnums,
    const std::vector<array>& outputs) {
  assert(primals.size() == 3);
  assert(outputs.size() == 1);
  assert(cotangents.size() == 1);

  auto s = stream();
  auto fallback = [eps = eps_, s](const std::vector<array>& inputs) {
    auto& x = inputs[0];
    auto& w = inputs[1];
    auto& b = inputs[2];
    auto& g = inputs[3];

    std::vector<array> vjps;

    auto norm = number_of_elements(x, {-1}, true, x.dtype(), s);
    auto sumx = sum(x, /* axis= */ -1, /* keepdims= */ true, s);
    auto sumx2 = sum(square(x, s), /* axis= */ -1, /* keepdims= */ true, s);
    auto mu = multiply(sumx, norm, s);
    auto mu2 = multiply(sumx2, norm, s);
    auto var = subtract(mu2, square(mu, s), s);
    auto n = rsqrt(add(var, array(eps, x.dtype()), s));
    auto n3 = power(n, array(3, x.dtype()), s);
    auto x_c = subtract(x, mu, s);

    // df/dx
    auto wg = multiply(w, g, s);
    auto sumwg =
        multiply(sum(wg, /* axis= */ -1, /* keepdims= */ true, s), norm, s);
    auto sumwgxc = multiply(
        sum(multiply(wg, x_c, s), /* axis= */ -1, /* keepdims= */ true, s),
        norm,
        s);
    auto t1 = multiply(multiply(x_c, sumwgxc, s), n3, s);
    auto t2 = multiply(subtract(wg, sumwg, s), n, s);
    vjps.push_back(subtract(t2, t1, s));

    // df/dw
    std::vector<int> axes(g.ndim() - 1);
    std::iota(axes.begin(), axes.end(), 0);
    if (w.ndim() == 0) {
      vjps.push_back(zeros_like(w, s));
    } else {
      vjps.push_back(sum(
          multiply(g, multiply(x_c, n, s), s), axes, /* keepdims= */ false, s));
    }

    // df/db
    if (b.ndim() == 0) {
      vjps.push_back(zeros_like(w, s));
    } else {
      vjps.push_back(sum(g, axes, /* keepdims= */ false, s));
    }

    return vjps;
  };

  auto vjps = array::make_arrays(
      {primals[0].shape(), primals[1].shape(), primals[2].shape()},
      {primals[0].dtype(), primals[1].dtype(), primals[2].dtype()},
      std::make_shared<LayerNormVJP>(s, fallback, eps_),
      {primals[0], primals[1], primals[2], cotangents[0]});

  std::vector<array> returned_vjps;
  for (auto& arg : argnums) {
    returned_vjps.push_back(std::move(vjps[arg]));
  }

  return returned_vjps;
}

bool LayerNorm::is_equivalent(const Primitive& other) const {
  const LayerNorm& a_other = static_cast<const LayerNorm&>(other);
  return eps_ == a_other.eps_;
}

bool LayerNormVJP::is_equivalent(const Primitive& other) const {
  const LayerNormVJP& a_other = static_cast<const LayerNormVJP&>(other);
  return eps_ == a_other.eps_;
}

array rope(
    std::vector<array> inputs,
    int dims,
    bool traditional,
    float base,
    float scale,
    bool forward,
    StreamOrDevice s) {
  auto& x = inputs[0];
  auto& offset = inputs[1];
  if (x.ndim() < 3) {
    std::ostringstream msg;
    msg << "[rope] Input must have at least 3 dimensions but got input with "
        << x.ndim() << " dimensions.";
    throw std::invalid_argument(msg.str());
  }
  if (!issubdtype(x.dtype(), floating)) {
    std::ostringstream msg;
    msg << "[rope] Input must be a floating type but got " << x.dtype() << ".";
    throw std::invalid_argument(msg.str());
  }
  if (offset.size() != 1) {
    std::ostringstream msg;
    msg << "[rope] offset must be a scalar but has shape " << offset.shape()
        << ".";
    throw std::invalid_argument(msg.str());
  }
  if (!issubdtype(offset.dtype(), integer)) {
    std::ostringstream msg;
    msg << "[rope] offset must be an integer but got type " << offset.dtype()
        << ".";
    throw std::invalid_argument(msg.str());
  }
  if (offset.dtype().size() != 4) {
    inputs[1] = astype(offset, uint32, s);
  }
  if (inputs.size() == 3 &&
      (inputs[2].ndim() != 1 || inputs[2].shape(0) != dims / 2)) {
    std::ostringstream msg;
    msg << "[rope] freqs must be one dimensional with size " << dims / 2
        << " but got shape " << inputs[2].shape() << ".";
    throw std::invalid_argument(msg.str());
  }

  auto fallback = [dims, traditional, base, scale, forward, s](
                      std::vector<array> inputs) {
    auto& shape = inputs[0].shape();
    int ndim = shape.size();
    auto x = flatten(inputs[0], 0, ndim - 3, s);
    auto t = x.dtype();
    // Compute sines and cosines
    auto half_dims = dims / 2;
    auto& offset = inputs[1];
    auto positions =
        multiply(add(arange(x.shape(1), t, s), offset, s), array(scale, t), s);

    auto default_inv_freqs = [&inputs, &s, &t, base, half_dims]() {
      return exp(
          multiply(
              arange(0, -half_dims, -1, t, s),
              array(std::log(base) / half_dims, t),
              s),
          s);
    };

    auto inv_freqs = inputs.size() == 3 ? astype(reciprocal(inputs[2], s), t, s)
                                        : default_inv_freqs();
    auto theta =
        multiply(expand_dims(positions, 1, s), expand_dims(inv_freqs, 0, s), s);
    auto coss = cos(theta, s);
    auto sins = sin(theta, s);

    auto apply_rope = [forward, s](
                          const array& x1,
                          const array& x2,
                          const array& coss,
                          const array& sins) {
      std::vector<array> outs;
      if (forward) {
        outs.push_back(
            subtract(multiply(x1, coss, s), multiply(x2, sins, s), s));
        outs.push_back(add(multiply(x1, sins, s), multiply(x2, coss, s), s));
      } else {
        outs.push_back(add(multiply(x2, sins, s), multiply(x1, coss, s), s));
        outs.push_back(
            subtract(multiply(x2, coss, s), multiply(x1, sins, s), s));
      }
      return outs;
    };

    if (traditional) {
      auto x1 =
          slice(x, {0, 0, 0}, {x.shape(0), x.shape(1), dims}, {1, 1, 2}, s);
      auto x2 =
          slice(x, {0, 0, 1}, {x.shape(0), x.shape(1), dims}, {1, 1, 2}, s);
      auto outs = apply_rope(x1, x2, coss, sins);
      for (auto& o : outs) {
        o = expand_dims(o, 3, s);
      }
      auto out = concatenate(outs, 3, s);
      if (dims < x.shape(-1)) {
        out = reshape(out, {x.shape(0), x.shape(1), dims});
        out = concatenate({out, slice(x, {0, 0, dims}, x.shape(), s)}, 2, s);
      }
      return std::vector<array>{reshape(out, shape, s)};
    } else {
      auto out_s = x.shape();
      out_s.back() = half_dims;
      auto x1 = slice(x, {0, 0, 0}, out_s, s);
      out_s.back() = dims;
      auto x2 = slice(x, {0, 0, half_dims}, out_s, s);

      auto outs = apply_rope(x1, x2, coss, sins);
      if (dims < x.shape(-1)) {
        outs.push_back(slice(x, {0, 0, dims}, x.shape(), s));
      }
      return std::vector<array>{reshape(concatenate(outs, 2, s), shape, s)};
    }
  };
  auto stream = to_stream(s);
  if (!RoPE::use_fallback(stream)) {
    return array(
        x.shape(),
        x.dtype(),
        std::make_shared<RoPE>(
            stream, fallback, dims, traditional, base, scale, forward),
        std::move(inputs));
  }
  return fallback(std::move(inputs))[0];
}

array rope(
    const array& x,
    int dims,
    bool traditional,
    std::optional<float> base,
    float scale,
    const array& offset,
    const std::optional<array>& freqs /* = std::nullopt */,
    StreamOrDevice s /* = {} */) {
  std::vector<array> inputs = {x, offset};
  if (freqs) {
    inputs.push_back(astype(*freqs, float32, s));
    if (base) {
      throw std::invalid_argument(
          "[rope] Only one of base or freqs can have a value.");
    }
  } else if (!base) {
    throw std::invalid_argument("[rope] Neither base nor freqs has a value.");
  }
  return rope(
      std::move(inputs),
      dims,
      traditional,
      base.has_value() ? *base : 1.0,
      scale,
      true,
      s);
}

array rope(
    const array& x,
    int dims,
    bool traditional,
    std::optional<float> base,
    float scale,
    int offset,
    const std::optional<array>& freqs /* = std::nullopt */,
    StreamOrDevice s /* = {} */) {
  return rope(
      x, dims, traditional, base, scale, array(offset, int32), freqs, s);
}

std::vector<array> RoPE::vjp(
    const std::vector<array>& primals,
    const std::vector<array>& cotangents,
    const std::vector<int>& argnums,
    const std::vector<array>& outputs) {
  auto s = stream();
  auto fallback = [dims = dims_,
                   traditional = traditional_,
                   base = base_,
                   scale = scale_,
                   forward = forward_,
                   s](std::vector<array> inputs) {
    return std::vector<array>{
        rope(std::move(inputs), dims, traditional, base, scale, !forward, s)};
  };
  if (argnums.size() > 1 || argnums[0] != 0) {
    throw std::invalid_argument(
        "[RoPE::vjp] vjp for offset or frequencies not supported");
  }
  auto inputs = std::vector<array>{cotangents[0], primals[1]};
  if (primals.size() == 3) {
    inputs.push_back(primals[2]);
  }
  return {array(
      cotangents[0].shape(),
      cotangents[0].dtype(),
      std::make_shared<RoPE>(
          s, fallback, dims_, traditional_, base_, scale_, !forward_),
      std::move(inputs))};
}

bool RoPE::is_equivalent(const Primitive& other) const {
  const RoPE& a_other = static_cast<const RoPE&>(other);
  return (
      dims_ == a_other.dims_ && base_ == a_other.base_ &&
      scale_ == a_other.scale_ && traditional_ == a_other.traditional_ &&
      forward_ == a_other.forward_);
}

/** Computes: O = softmax(Q @ K.T) @ V **/
array scaled_dot_product_attention(
    const array& queries,
    const array& keys,
    const array& values,
    const float scale,
    const std::string& mask_mode /* = "" */,
    const std::vector<array>& mask_arrs /* = {} */,
    StreamOrDevice s /* = {}*/) {
  for (const auto& tensor : {queries, keys, values}) {
    if (tensor.ndim() != 4) {
      std::ostringstream msg;
      msg << "[scaled_dot_product_attention] input with shape "
          << tensor.shape() << " expected to be rank 4";
      throw std::invalid_argument(msg.str());
    }
  }
  // Check valid mask
  if (mask_mode != "" && mask_mode != "causal" && mask_mode != "array") {
    std::ostringstream msg;
    msg << "[scaled_dot_product_attention] Invalid mask_mode " << mask_mode
        << ". mask_mode must be 'causal', 'array' or ''.";
    throw std::invalid_argument(msg.str());
  }

  bool do_causal = false;
  bool has_mask = false;
  bool has_arr_mask = false;
  bool has_bool_mask = false;

  if (mask_mode == "causal") {
    has_mask = true;
    do_causal = true;

    if (!mask_arrs.empty()) {
      std::ostringstream msg;
      msg << "[scaled_dot_product_attention] Invalid mask_arrs for mask_mode "
          << "'casusal'. No array masks supported.";
      throw std::invalid_argument(msg.str());
    }
  }

  if (mask_mode == "array" || (mask_mode == "" && !mask_arrs.empty())) {
    if (mask_arrs.size() != 1) {
      std::ostringstream msg;
      msg << "[scaled_dot_product_attention] Invalid mask_arrs for mask_mode "
          << "'" << mask_mode << "'. Only 1 mask array is supported, got "
          << mask_arrs.size() << "arrays.";
      throw std::invalid_argument(msg.str());
    }

    has_mask = true;
    has_arr_mask = true;
    has_bool_mask = mask_arrs[0].dtype() == bool_;
  }

  if (has_arr_mask && (mask_arrs[0]).ndim() > 4) {
    std::ostringstream msg;
    msg << "[scaled_dot_product_attention] the mask with shape "
        << mask_arrs[0].shape() << " expected to have at most rank 4.";
    throw std::invalid_argument(msg.str());
  }

  const size_t batch_dim = queries.shape(0);
  for (const auto& tensor : {keys, values}) {
    if (tensor.shape(0) != batch_dim) {
      std::ostringstream msg;
      msg << "[scaled_dot_product_attention] mismatching batch dimension for input with shape "
          << tensor.shape() << ".";
      throw std::invalid_argument(msg.str());
    }
  }

  // Q, K must have matching last dims (d_k aka 'head_dim');
  if (queries.shape(-1) != keys.shape(-1)) {
    std::ostringstream msg;
    msg << "[scaled_dot_product_attention] query, keys expected to have matching last dimension; found query shape "
        << queries.shape() << " for keys shape " << keys.shape() << ".";
    throw std::invalid_argument(msg.str());
  }

  // K, V must have matching number of heads (n_kv_heads);
  auto n_q_heads = queries.shape(-3);
  auto n_kv_heads = keys.shape(-3);

  if (keys.shape(-3) != values.shape(-3)) {
    std::ostringstream msg;
    msg << "[scaled_dot_product_attention] keys, values expected to have matching n_kv_heads; found keys with n_heads "
        << keys.shape(-3) << " for values with n_heads " << values.shape(-3)
        << ".";
    throw std::invalid_argument(msg.str());
  }

  // n_heads % n_kv_heads == 0; n_heads >= 1, n_kv_heads >= 1.
  if (n_q_heads % n_kv_heads != 0) {
    std::ostringstream msg;
    msg << "[scaled_dot_product_attention] n_heads must be a multiple of n_kv_heads, found n_heads "
        << n_q_heads << " for n_kv_heads " << n_kv_heads << ".";
    throw std::invalid_argument(msg.str());
  }

  auto final_type = result_type(queries, keys, values);
  if (!issubdtype(final_type, floating)) {
    std::ostringstream msg;
    msg << "[scaled_dot_product_attention] Received unsupported type "
        << final_type << ".";
    throw std::invalid_argument(msg.str());
  }

  auto q = astype(queries, final_type, s);
  auto k = astype(keys, final_type, s);
  auto v = astype(values, final_type, s);

  auto fallback = [scale, final_type, n_q_heads, n_kv_heads, do_causal, s](
                      const std::vector<array>& inputs) {
    auto q = multiply(array(scale, inputs[0].dtype()), inputs[0], s);
    int n_repeats = n_q_heads / n_kv_heads;
    int B = q.shape(0);
    int L = q.shape(2);
    auto k = inputs[1];
    auto v = inputs[2];
    if (n_repeats > 1) {
      q = unflatten(q, 1, {n_kv_heads, n_repeats}, s);
      k = expand_dims(k, 2, s);
      v = expand_dims(v, 2, s);
    }
    auto scores = matmul(q, swapaxes(k, -1, -2, s), s);
    if (inputs.size() > 3 || do_causal) {
      // Mask must be broadcast-compatible with [B, n_q_heads, L_q, L_kv]
      auto mask = inputs.back();

      if (do_causal) {
        int kL = k.shape(-2);
        int qL = q.shape(-2);
        int q_off = (kL - qL) < 0 ? 0 : (kL - qL);
        auto q_idx = arange(q_off, q_off + qL, s);
        auto k_idx = arange(0, kL, s);
        q_idx = expand_dims(q_idx, 1, s);
        k_idx = expand_dims(k_idx, 0, s);
        mask = greater_equal(q_idx, k_idx, s);
      }

      if (n_repeats > 1 && mask.ndim() >= 3) {
        if (mask.shape(-3) == 1) {
          mask = expand_dims(mask, -3, s);
        } else {
          mask = unflatten(mask, -3, {n_kv_heads, n_repeats}, s);
        }
      }
      if (mask.dtype() == bool_) {
        scores = where(
            mask,
            scores,
            array(-std::numeric_limits<float>::infinity(), scores.dtype()),
            s);
      } else {
        scores = add(scores, mask, s);
      }
    }
    scores = softmax(scores, std::vector<int>{-1}, true, s);
    auto out = matmul(scores, v, s);
    if (n_repeats > 1) {
      out = flatten(out, 1, 2, s);
    }
    return std::vector<array>{out};
  };

  auto stream = to_stream(s);
  std::vector<array> inputs = {q, k, v};
  if (has_arr_mask) {
    // Check type
    auto mask_arr = mask_arrs[0];
    has_bool_mask = mask_arr.dtype() == bool_;
    if (promote_types(mask_arr.dtype(), final_type) != final_type) {
      std::ostringstream msg;
      msg << "[scaled_dot_product_attention] Mask type must promote to output type. "
          << final_type << ".";
      throw std::invalid_argument(msg.str());
    } else if (!has_bool_mask) {
      mask_arr = astype(mask_arr, final_type, stream);
    }
    // Broadcast mask
    auto mask_shape = queries.shape();
    mask_shape.back() = keys.shape(-2);
    inputs.push_back(broadcast_to(mask_arr, mask_shape, stream));
  }
  if (!ScaledDotProductAttention::use_fallback(
          q, k, v, has_mask, has_arr_mask, do_causal, stream)) {
    auto out_shape = Shape{q.shape(0), q.shape(1), q.shape(2), v.shape(-1)};
    return array(
        std::move(out_shape),
        final_type,
        std::make_shared<ScaledDotProductAttention>(
            stream, fallback, scale, do_causal),
        std::move(inputs));
  }
  return fallback(std::move(inputs))[0];
}

bool ScaledDotProductAttention::is_equivalent(const Primitive& other) const {
  const ScaledDotProductAttention& a_other =
      static_cast<const ScaledDotProductAttention&>(other);
  return scale_ == a_other.scale_ && do_causal_ == a_other.do_causal_;
}

array pack_and_quantize(
    array& packed_w,
    const array& scales,
    const array& biases,
    int bits,
    const Stream& s) {
  int el_per_int = 32 / bits;
  array zero(0, packed_w.dtype());
  array n_bins((1 << bits) - 1, packed_w.dtype()); // 2**bits - 1
  packed_w = astype(
      clip(
          round(divide(subtract(packed_w, biases, s), scales, s), s),
          zero,
          n_bins,
          s),
      uint32,
      s);
  if (is_power_of_2(bits)) {
    array shifts = power(array(2, uint32), arange(0, 32, bits, uint32, s), s);
    packed_w = reshape(packed_w, {packed_w.shape(0), -1, el_per_int}, s);
    packed_w = sum(
        multiply(packed_w, shifts, s), /* axis= */ 2, /* keepdims= */ false, s);
  } else {
    // This is slow but we have fast GPU/CPU versions of this function so we
    // shouldn't be here often.
    packed_w = expand_dims(packed_w, /* axis= */ -1, s);
    packed_w = bitwise_and(
        right_shift(packed_w, arange(bits, uint32, s), s),
        array({1}, uint32),
        s);
    auto new_shape = packed_w.shape();
    new_shape[new_shape.size() - 2] = -1;
    new_shape.back() = 32;
    packed_w = reshape(packed_w, new_shape, s);
    array shifts = arange(32, uint32, s);
    packed_w =
        sum(left_shift(packed_w, shifts, s),
            /* axis= */ -1,
            /* keepdims= */ false,
            s);
  }
  return packed_w;
}

std::tuple<array, array, array>
affine_quantize(const array& w, int group_size, int bits, StreamOrDevice s_) {
  auto s = to_stream(s_);

  if (group_size != 32 && group_size != 64 && group_size != 128) {
    std::ostringstream msg;
    msg << "[quantize] The requested group size " << group_size
        << " is not supported. The supported group sizes are 32, 64, and 128.";
    throw std::invalid_argument(msg.str());
  }

  if (bits < 2 || bits > 8 || bits == 7) {
    std::ostringstream msg;
    msg << "[quantize] The requested number of bits " << bits
        << " is not supported. The supported bits are 2, 3, 4, 5, 6 and 8.";
    throw std::invalid_argument(msg.str());
  }

  if (w.ndim() < 2) {
    std::ostringstream msg;
    msg << "[quantize] The matrix to be quantized must have at least 2 dimension "
        << "but it has only " << w.ndim() << ".";
    throw std::invalid_argument(msg.str());
  }

  if ((w.shape(-1) % group_size) != 0) {
    std::ostringstream msg;
    msg << "[quantize] The last dimension of the matrix needs to be divisible by "
        << "the quantization group size " << group_size
        << ". However the provided " << " matrix has shape " << w.shape();
    throw std::invalid_argument(msg.str());
  }

  auto fallback = [group_size, bits, s](
                      const std::vector<array>& inputs) -> std::vector<array> {
    auto& w = inputs[0];
    auto wshape = w.shape();
    wshape.back() = -1;

    array zero(0, float32);
    array n_bins((1 << bits) - 1, float32); // 2**bits - 1
    array eps(1e-7, float32);

    array packed_w = reshape(w, {-1, w.shape(-1) / group_size, group_size}, s);

    array w_max = max(packed_w, /* axis= */ -1, /* keepdims= */ true, s);
    array w_min = min(packed_w, /* axis= */ -1, /* keepdims= */ true, s);
    w_max = astype(w_max, float32, s);
    w_min = astype(w_min, float32, s);

    array mask = greater(abs(w_min, s), abs(w_max, s), s);
    array scales =
        maximum(divide(subtract(w_max, w_min, s), n_bins, s), eps, s);
    scales = where(mask, scales, negative(scales, s), s);
    array edge = where(mask, w_min, w_max, s);
    array q0 = round(divide(edge, scales, s), s);
    scales = where(not_equal(q0, zero, s), divide(edge, q0, s), scales);
    array biases = where(equal(q0, zero, s), zero, edge, s);

    packed_w = pack_and_quantize(packed_w, scales, biases, bits, s);

    scales = astype(scales, w.dtype(), s);
    biases = astype(biases, w.dtype(), s);
    return {
        reshape(packed_w, wshape, s),
        reshape(scales, wshape, s),
        reshape(biases, wshape, s),
    };
  };

  auto wq_shape = w.shape();
  wq_shape.back() = w.shape(-1) * bits / 32;
  auto sshape = w.shape();
  sshape.back() = w.shape(-1) / group_size;
  auto outputs = array::make_arrays(
      {std::move(wq_shape), sshape, sshape},
      {uint32, w.dtype(), w.dtype()},
      std::make_shared<AffineQuantize>(s, fallback, group_size, bits, false),
      {w});
  return {outputs[0], outputs[1], outputs[2]};
}

array affine_dequantize(
    const array& w,
    const array& scales,
    const array& biases,
    int group_size,
    int bits,
    StreamOrDevice s_) {
  if (bits <= 0) {
    std::ostringstream msg;
    msg << "[dequantize] Invalid value for bits: " << bits;
    throw std::invalid_argument(msg.str());
  }
  if (group_size <= 0) {
    std::ostringstream msg;
    msg << "[dequantize] Invalid value for group_size: " << group_size;
    throw std::invalid_argument(msg.str());
  }
  if (w.ndim() < 2 || scales.ndim() < 2 || biases.ndim() < 2) {
    std::ostringstream msg;
    msg << "[quantize] The matrix to be quantized must have at least 2 dimension "
        << "but it has only " << w.ndim() << ".";
    throw std::invalid_argument(msg.str());
  }

  auto wshape = w.shape();
  auto sshape = scales.shape();
  auto bshape = biases.shape();
  wshape.back() = -1;
  sshape.back() = -1;
  bshape.back() = -1;

  if (wshape != sshape || wshape != bshape) {
    throw std::invalid_argument(
        "[dequantize] Shape of scales and biases does not match the matrix");
  }

  if (w.dtype() != uint32) {
    throw std::invalid_argument(
        "[dequantize] The matrix should be given as a uint32");
  }

  // Packing into uint32
  int out_size = w.shape(-1) * 32 / bits;

  if (out_size != scales.shape(-1) * group_size) {
    std::ostringstream msg;
    msg << "[dequantize] Shape of scales and biases does not match the matrix "
        << "given the quantization parameters. Provided matrix of shape "
        << w.shape() << " and scales/biases of shape " << scales.shape()
        << " with group_size=" << group_size << " and bits=" << bits << ".";
    throw std::invalid_argument(msg.str());
  }

  auto s = to_stream(s_);

  auto fallback =
      [wshape = std::move(wshape),
       sshape = std::move(sshape),
       group_size,
       bits,
       s](const std::vector<array>& inputs) mutable -> std::vector<array> {
    auto w = inputs[0];
    auto& scales = inputs[1];
    auto& biases = inputs[2];
    if (is_power_of_2(bits)) {
      std::vector<array> parts;
      for (int start = 0; start < 32; start += bits) {
        int shift_left = 32 - (start + bits);
        int shift_right = shift_left + start;

        parts.push_back(expand_dims(
            right_shift(
                left_shift(w, array(32 - (start + bits), uint32), s),
                array(32 - bits, uint32),
                s),
            -1,
            s));
      }
      w = concatenate(parts, -1, s);
    } else {
      w = expand_dims(w, /* axis= */ -1, s);
      w = bitwise_and(
          right_shift(w, arange(32, uint32, s), s), array({1}, uint32), s);
      auto new_shape = w.shape();
      new_shape[new_shape.size() - 2] = -1;
      new_shape.back() = bits;
      w = reshape(w, new_shape, s);
      array shifts = arange(bits, uint32, s);
      w = sum(
          left_shift(w, shifts, s), /* axis= */ -1, /* keepdims= */ false, s);
    }

    // Dequantize
    wshape.push_back(group_size);
    w = reshape(w, wshape, s);
    w = multiply(w, expand_dims(scales, -1, s), s);
    w = add(w, expand_dims(biases, -1, s), s);
    w = reshape(w, sshape, s);

    return {w};
  };

  if (s.device == Device::gpu) {
    auto out_shape = w.shape();
    out_shape.back() = out_size;
    return array(
        std::move(out_shape),
        scales.dtype(),
        std::make_shared<AffineQuantize>(s, fallback, group_size, bits, true),
        {w, scales, biases});
  }
  return fallback({w, scales, biases})[0];
}

bool AffineQuantize::is_equivalent(const Primitive& other) const {
  const AffineQuantize& p_other = static_cast<const AffineQuantize&>(other);
  return (
      p_other.group_size_ == group_size_ && p_other.bits_ == bits_ &&
      p_other.dequantize_ == dequantize_);
}

std::vector<Shape> AffineQuantize::output_shapes(
    const std::vector<array>& inputs) {
  auto& w = inputs[0];
  if (dequantize_) {
    auto out_size = w.shape(-1) * 32 / bits_;
    auto out_shape = w.shape();
    out_shape.back() = out_size;
    return {std::move(out_shape)};
  } else {
    auto wq_shape = w.shape();
    wq_shape.back() = w.shape(-1) * bits_ / 32;
    auto sshape = w.shape();
    sshape.back() = w.shape(-1) / group_size_;
    auto bshape = sshape;
    return {std::move(wq_shape), std::move(sshape), std::move(bshape)};
  }
}

} // namespace mlx::core::fast
