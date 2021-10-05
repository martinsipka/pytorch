#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>
#include <ATen/Dispatch.h>
#include <ATen/CPUApplyUtils.h>
#include <ATen/native/BinaryOps.h>
#include <ATen/native/PointwiseOps.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/cpu/Loops.h>
#include <ATen/native/Resize.h>
#include <c10/core/MemoryFormat.h>
#include <c10/core/ScalarType.h>

constexpr float EPSILON = 1e-12;

namespace {
  static inline at::Tensor apply_loss_reduction(const at::Tensor& unreduced, int64_t reduction) {
    if (reduction == at::Reduction::Mean) {
      return unreduced.mean();
    } else if (reduction == at::Reduction::Sum) {
      return unreduced.sum();
    }
    return unreduced;
  }
}

namespace at { namespace native {

DEFINE_DISPATCH(l1_stub);
DEFINE_DISPATCH(l1_backward_stub);
DEFINE_DISPATCH(smooth_l1_stub);
DEFINE_DISPATCH(smooth_l1_backward_stub);
DEFINE_DISPATCH(huber_stub);
DEFINE_DISPATCH(huber_backward_stub);
DEFINE_DISPATCH(mse_stub);
DEFINE_DISPATCH(mse_backward_stub);

Tensor cosine_embedding_loss(const Tensor& input1, const Tensor& input2, const Tensor& target, double margin, int64_t reduction) {
  auto targ_dim = target.dim();
  TORCH_CHECK(
      targ_dim == 1 || targ_dim == 0,
      "0D or 1D target tensor expected, multi-target not supported");

  if (targ_dim == 1) {
    TORCH_CHECK(
        input1.dim() == 2,
        "1D target tensor expects 2D input tensors, but found inputs with sizes ",
        input1.sizes(),
        " and ",
        input2.sizes(),
        ".");
  } else {
    TORCH_CHECK(
        input1.dim() == 1,
        "0D target tensor expects 1D input tensors, but found inputs with sizes ",
        input1.sizes(),
        " and ",
        input2.sizes(),
        ".");
  }

  auto prod_sum = (input1 * input2).sum(targ_dim);
  auto mag_square1 = (input1 * input1).sum(targ_dim) + EPSILON;
  auto mag_square2 = (input2 * input2).sum(targ_dim) + EPSILON;
  auto denom = (mag_square1 * mag_square2).sqrt_();
  auto cos = prod_sum / denom;

  auto zeros = at::zeros_like(cos, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
  auto pos = 1 - cos;
  auto neg = (cos - margin).clamp_min_(0);
  auto output_pos = at::where(target == 1, pos, zeros);
  auto output_neg = at::where(target == -1, neg, zeros);
  auto output = output_pos + output_neg;
  return apply_loss_reduction(output, reduction);
}

Tensor hinge_embedding_loss(const Tensor& self, const Tensor& target, double margin, int64_t reduction) {
  auto zeros = at::zeros_like(self, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
  auto margin_clamp = (margin - self).clamp_min_(0);
  auto output_margin = at::where(target != 1, margin_clamp, zeros);
  auto output_self = at::where(target != -1, self, zeros);
  auto output = output_margin + output_self;
  return apply_loss_reduction(output, reduction);
}

Tensor triplet_margin_loss(const Tensor& anchor, const Tensor& positive, const Tensor& negative, double margin,
                           double p, double eps, bool swap, int64_t reduction) {
  auto a_dim = anchor.dim();
  auto p_dim = positive.dim();
  auto n_dim = negative.dim();
  TORCH_CHECK(
      a_dim == p_dim && p_dim == n_dim,
      "All inputs should have same dimension but got ",
      a_dim,
      "D, ",
      p_dim,
      "D and ",
      n_dim,
      "D inputs.")
  auto dist_pos = at::pairwise_distance(anchor, positive, p, eps);
  auto dist_neg = at::pairwise_distance(anchor, negative, p, eps);
  if (swap) {
    auto dist_swap = at::pairwise_distance(positive, negative, p, eps);
    dist_neg = at::min(dist_neg, dist_swap);
  }
  auto output = at::clamp_min(margin + dist_pos - dist_neg, 0);
  return apply_loss_reduction(output, reduction);
}

Tensor margin_ranking_loss(const Tensor& input1, const Tensor& input2, const Tensor& target, double margin, int64_t reduction) {
  auto output =  (-target * (input1 - input2) + margin).clamp_min_(0);
  return apply_loss_reduction(output, reduction);
}

Tensor kl_div(const Tensor& input, const Tensor& target, int64_t reduction, bool log_target) {
  Tensor output;
  if (log_target) {
    output = at::exp(target) * (target - input);
  }
  else {
    // continuous extension 0 * log(0) := 0
    auto output_not_extended = target * (at::log(target) - input);
    auto zeros = at::zeros_like(output_not_extended);
    output = at::where(target == 0, zeros, output_not_extended);
  }
  return apply_loss_reduction(output, reduction);
}

Tensor binary_cross_entropy_cpu(const Tensor& input, const Tensor& target, const c10::optional<Tensor>& weight_opt, int64_t reduction) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;

    Tensor loss = at::empty_like(input);
    return at::native::binary_cross_entropy_out_cpu(
        input, target, weight, reduction, loss);
}

Tensor& binary_cross_entropy_out_cpu(const Tensor& input, const Tensor& target, const c10::optional<Tensor>& weight_opt, int64_t reduction, Tensor& loss) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;

    Tensor loss_squeezed = at::squeeze(loss);

    auto iter = TensorIteratorConfig()
      .add_output(loss_squeezed)
      .add_owned_input(at::squeeze(input))
      .add_owned_input(at::squeeze(target))
      .build();

    AT_DISPATCH_FLOATING_TYPES(loss.scalar_type(), "binary_cross_entropy", [&] {
        at::native::cpu_kernel(
            iter,
            [] (scalar_t input_val, scalar_t target_val) {
                TORCH_CHECK(
                    (input_val >= 0) && (input_val <= 1),
                    "all elements of input should be between 0 and 1"
                );

                // Binary cross entropy tensor is defined by the equation:
                // L = -w (y ln(x) + (1-y) ln(1-x))
                return (target_val - scalar_t(1))
                    * std::max(scalar_t(std::log(scalar_t(1) - input_val)), scalar_t(-100))
                    - target_val * std::max(scalar_t(std::log(input_val)), scalar_t(-100));
            }
        );
    });
    if (weight.defined()) {
        loss.mul_(weight);
    }
    if (reduction != at::Reduction::None) {
        Tensor loss_reduced = apply_loss_reduction(loss, reduction);
        loss.resize_as_(loss_reduced).copy_(loss_reduced);
    }
    return loss;
}

Tensor binary_cross_entropy_backward_cpu(const Tensor& grad, const Tensor& input, const Tensor& target, const c10::optional<Tensor>& weight_opt, int64_t reduction) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;

    Tensor grad_input = at::empty_like(input);
    return at::native::binary_cross_entropy_backward_out_cpu(
        grad, input, target, weight, reduction, grad_input);
}

Tensor& binary_cross_entropy_backward_out_cpu(const Tensor& grad, const Tensor& input, const Tensor& target, const c10::optional<Tensor>& weight_opt, int64_t reduction, Tensor& grad_input) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;

    Tensor grad_input_squeezed = at::squeeze(grad_input);

    auto iter = TensorIteratorConfig()
      .add_output(grad_input_squeezed)
      .add_owned_input(at::squeeze(grad))
      .add_owned_input(at::squeeze(input))
      .add_owned_input(at::squeeze(target))
      .build();

    AT_DISPATCH_FLOATING_TYPES(grad_input.scalar_type(), "binary_cross_entropy_backward", [&] {
        at::native::cpu_kernel(
            iter,
            [] (scalar_t grad_val, scalar_t input_val, scalar_t target_val) {
                // The gradient is the partial derivative of BCELoss
                // with respect to x
                // d(L)/d(x) = -w (y - x) / (x - x^2)
                return grad_val * (input_val - target_val)
                    / (scalar_t(std::max(
                        (scalar_t(1) - input_val) * input_val,
                        scalar_t(EPSILON)
                    )));
            }
        );
    });
    if (weight.defined()) {
        grad_input.mul_(weight);
    }
    if (reduction == at::Reduction::Mean) {
        grad_input.div_(input.numel());
    }
    return grad_input;
}

Tensor binary_cross_entropy_with_logits(const Tensor& input, const Tensor& target, const c10::optional<Tensor>& weight_opt, const c10::optional<Tensor>& pos_weight_opt, int64_t reduction) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  const Tensor& pos_weight = c10::value_or_else(pos_weight_opt, [] {return Tensor();});

    Tensor loss;
    auto max_val = (-input).clamp_min_(0);
    if (pos_weight.defined()) {
        // pos_weight need to be broadcasted, thus mul(target) is not inplace.
        auto log_weight = (pos_weight - 1).mul(target).add_(1);
        loss = (1 - target).mul_(input).add_(log_weight.mul_(((-max_val).exp_().add_((-input - max_val).exp_())).log_().add_(max_val)));
    } else {
        loss = (1 - target).mul_(input).add_(max_val).add_((-max_val).exp_().add_((-input -max_val).exp_()).log_());
    }

    if (weight.defined()) {
        loss.mul_(weight);
    }

    return apply_loss_reduction(loss, reduction);
}

Tensor binary_cross_entropy_with_logits_backward(const Tensor& grad, const Tensor& input, const Tensor& target, const c10::optional<Tensor>& weight_opt, const c10::optional<Tensor>& pos_weight_opt, int64_t reduction) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  const Tensor& pos_weight = c10::value_or_else(pos_weight_opt, [] {return Tensor();});

    Tensor grad_input;
    if (pos_weight.defined()) {
        // pos_weight need to be broadcasted, thus mul(target) is not inplace.
        auto t = pos_weight.mul(target);
        grad_input = t.add(1).sub_(target).mul_(input.sigmoid()).sub_(t).mul_(grad);
    } else {
        grad_input = (input.sigmoid() - target).mul_(grad);
    }

    if (weight.defined()) {
        grad_input.mul_(weight);
    }

    if (reduction == at::Reduction::Mean) {
        return grad_input / input.numel();
    }

    return grad_input;
}

Tensor poisson_nll_loss(const Tensor& input, const Tensor& target, const bool log_input, const bool full, const double eps, const int64_t reduction)
{
    Tensor loss;
    if (log_input) {
        loss = at::exp(input) - target * input;
    } else {
        loss = input - target * at::log(input + eps);
    }

    if (full) {
        auto stirling_term = target * at::log(target) - target + 0.5 * at::log(2 * c10::pi<double> * target);
        loss += stirling_term.masked_fill(target <= 1, 0);
    }

    return apply_loss_reduction(loss, reduction);
}

Tensor& soft_margin_loss_backward_out(const Tensor& grad_output, const Tensor& input, const Tensor& target, int64_t reduction, Tensor& grad_input) {
  auto norm = reduction == Reduction::Mean ? 1. / input.numel() : 1.;
  auto z = at::exp(-target * input);
  // inplace version of: grad_input = -norm * target * z / (1. + z) * grad_output;
  at::mul_out(grad_input, target, z).mul_(-norm);
  z.add_(1);
  grad_input.div_(z).mul_(grad_output);
  return grad_input;
}

Tensor soft_margin_loss_backward(const Tensor& grad_output, const Tensor& input, const Tensor& target, int64_t reduction) {
  auto grad_input = at::empty({0}, input.options());
  at::soft_margin_loss_backward_out(grad_input, grad_output, input, target, reduction);
  return grad_input;
}

Tensor& soft_margin_loss_out(const Tensor& input,
    const Tensor& target,
    int64_t reduction,
    Tensor& output) {
  // compute inplace variant of: output = at::log(1. + at::exp(-input * target));
  at::neg_out(output, input).mul_(target).exp_().add_(1.).log_();
  if (reduction != Reduction::None) {
    auto tmp = apply_loss_reduction(output, reduction);
    output.resize_({});
    output.copy_(tmp);
  }
  return output;
}

Tensor soft_margin_loss(
    const Tensor& input,
    const Tensor& target,
    int64_t reduction) {
  auto output = at::empty({0}, input.options());
  at::soft_margin_loss_out(output, input, target, reduction);
  return output;
}

Tensor& l1_loss_out(const Tensor& input, const Tensor& target, int64_t reduction, Tensor& result) {
  const auto common_type = promoteTypes(input.scalar_type(), target.scalar_type());
  const auto reduce = reduction != Reduction::None;
  const auto is_complex = isComplexType(common_type);
  const auto aux_output = reduce || is_complex;
  auto output_iter =  aux_output ? MaybeOwned<Tensor>::owned(at::empty({0}, input.options().dtype(common_type)))
                                 : MaybeOwned<Tensor>::borrowed(result);
  auto iter = TensorIterator::borrowing_binary_op(*output_iter, input, target);

  l1_stub(iter.device_type(), iter);

  // No need to resize the output otherwise, as TensorIterator will take care of that
  if (reduce) {
    at::native::resize_output(result, {});
  } else if (is_complex) {
    at::native::resize_output(result, iter.shape());
  }

  if (is_complex) {
    if (reduction == Reduction::Mean) {
      result.copy_(at::real(output_iter->mean()));
    } else if (reduction == Reduction::Sum) {
      result.copy_(at::real(output_iter->sum()));
    } else {
      result.copy_(at::real(*output_iter));
    }
  } else {
    if (reduction == Reduction::Mean) {
      result.copy_(output_iter->mean());
    } else if (reduction == Reduction::Sum) {
      result.copy_(output_iter->sum());
    }
  }
  return result;
}

Tensor l1_loss(const Tensor& input, const Tensor& target, const int64_t reduction) {
  const auto real_type = toValueType(promoteTypes(input.scalar_type(), target.scalar_type()));
  Tensor output = at::empty({0}, input.options().dtype(real_type));
  at::native::l1_loss_out(input, target, reduction, output);
  return output;
}

Tensor& smooth_l1_loss_out(const Tensor& input, const Tensor& target, int64_t reduction, double beta, Tensor& result) {
  TORCH_CHECK(beta >= 0, "smooth_l1_loss does not support negative values for beta.")
  if (beta == 0) {
    return at::native::l1_loss_out(input, target, reduction, result);
  }
  const auto common_type = promoteTypes(input.scalar_type(), target.scalar_type());
  const auto reduce = reduction != Reduction::None;
  auto output_iter =  reduce ? MaybeOwned<Tensor>::owned(at::empty({0}, input.options().dtype(common_type)))
                             : MaybeOwned<Tensor>::borrowed(result);
  auto iter = TensorIterator::borrowing_binary_op(*output_iter, input, target);

  // No need to resize the output otherwise, as TensorIterator will take care of that
  if (reduce) {
    at::native::resize_output(result, {});
  }

  smooth_l1_stub(iter.device_type(), iter, beta);

  if (reduction == Reduction::Mean) {
    result.copy_(output_iter->mean());
  } else if (reduction == Reduction::Sum) {
    result.copy_(output_iter->sum());
  }

  return result;
}

Tensor smooth_l1_loss(const Tensor& input, const Tensor& target, const int64_t reduction, double beta) {
  const auto common_type = promoteTypes(input.scalar_type(), target.scalar_type());
  Tensor output = at::empty({0}, input.options().dtype(common_type));
  at::native::smooth_l1_loss_out(input, target, reduction, beta, output);
  return output;
}

Tensor l1_loss_backward(const Tensor& grad_output, const Tensor& input, const Tensor& target, int64_t reduction) {
  const auto common_type = promoteTypes(input.scalar_type(), target.scalar_type());
  auto grad_input = at::empty(input.sizes(), input.options().dtype(common_type));
  auto iter = at::TensorIteratorConfig()
    .add_output(grad_input)
    .add_input(input)
    .add_input(target)
    .add_input(grad_output)
    .promote_inputs_to_common_dtype(true)
    .cast_common_dtype_to_outputs(true)
    .enforce_safe_casting_to_output(true)
    .build();
  auto norm = reduction == Reduction::Mean ? 1. / iter.numel() : 1.;
  l1_backward_stub(iter.device_type(), iter, norm);
  return grad_input;
}

Tensor smooth_l1_loss_backward(const Tensor& grad_output, const Tensor& input, const Tensor& target, int64_t reduction, double beta) {
  if (beta == 0) {
    return at::native::l1_loss_backward(grad_output, input, target, reduction);
  }
  auto grad_input = at::zeros_like(input, at::MemoryFormat::Preserve);
  auto iter = at::TensorIteratorConfig()
    .add_output(grad_input)
    .add_input(input)
    .add_input(target)
    .add_input(grad_output)
    .promote_inputs_to_common_dtype(true)
    .build();
  auto norm = reduction == Reduction::Mean ? 1. / iter.numel() : 1.;
  smooth_l1_backward_stub(iter.device_type(), iter, norm, beta);
  return grad_input;
}

Tensor huber_loss(const Tensor& input, const Tensor& target, int64_t reduction, double delta) {
  TORCH_CHECK(delta > 0, "huber_loss does not support non-positive values for delta.")
  Tensor loss = at::empty_like(input);
  auto iter = TensorIterator::borrowing_binary_op(loss, input, target);
  huber_stub(iter.device_type(), iter, delta);
  return apply_loss_reduction(loss, reduction);
}

Tensor& huber_loss_out(const Tensor& input, const Tensor& target, int64_t reduction, double delta, Tensor& result) {
  TORCH_CHECK(delta > 0, "huber_loss does not support non-positive values for delta.")
  auto iter = TensorIterator::borrowing_binary_op(result, input, target);
  huber_stub(iter.device_type(), iter, delta);
  if (reduction != Reduction::None) {
    auto reduced = apply_loss_reduction(result, reduction);
    result.resize_({});
    result.copy_(reduced);
  }
  return result;
}

Tensor huber_loss_backward(const Tensor& grad_output, const Tensor& input, const Tensor& target, int64_t reduction, double delta) {
  auto grad_input = at::zeros_like(input, MemoryFormat::Contiguous);
  return at::huber_loss_backward_out(grad_input, grad_output, input, target, reduction, delta);
}

Tensor& huber_loss_backward_out(const Tensor& grad_output, const Tensor& input, const Tensor& target, int64_t reduction, double delta, Tensor& grad_input) {
  auto norm = (reduction == Reduction::Mean) ? (1. / input.numel()) : 1.;
  auto iter = at::TensorIteratorConfig()
    .add_output(grad_input)
    .add_input(input)
    .add_input(target)
    .add_input(grad_output)
    .build();
  huber_backward_stub(iter.device_type(), iter, norm, delta);
  return grad_input;
}

Tensor mse_loss(const Tensor& input, const Tensor& target, int64_t reduction) {
  Tensor loss;
  auto iter = TensorIterator::borrowing_binary_op(loss, input, target);
  mse_stub(iter.device_type(), iter);
  return apply_loss_reduction(iter.output(), reduction);
}

Tensor& mse_loss_out(const Tensor& input, const Tensor& target, int64_t reduction, Tensor&result) {
  if (reduction != Reduction::None) {
    Tensor loss;
    auto iter = TensorIterator::borrowing_binary_op(loss, input, target);
    mse_stub(iter.device_type(), iter);
    if (reduction == Reduction::Mean) {
      at::mean_out(result, iter.output(), 0);
    } else {
      at::sum_out(result, iter.output(), 0);
    }
  } else {
    auto iter = TensorIterator::borrowing_binary_op(result, input, target);
    mse_stub(iter.device_type(), iter);
  }
  return result;
}

Tensor mse_loss_backward(const Tensor& grad_output, const Tensor& input, const Tensor& target, int64_t reduction) {
  Tensor grad_input = at::zeros_like(input, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
  return at::mse_loss_backward_out(grad_input, grad_output, input, target, reduction);
}

Tensor& mse_loss_backward_out(const Tensor& grad_output,
    const Tensor& input, const Tensor& target, int64_t reduction, Tensor& grad_input) {
  auto norm = reduction == Reduction::Mean ? 2. / input.numel() : 2.;
  auto iter = at::TensorIteratorConfig()
    .add_output(grad_input)
    .add_input(input)
    .add_input(target)
    .add_input(grad_output)
    .build();
  mse_backward_stub(iter.device_type(), iter, norm);
  return grad_input;
}

}}  // namespace at::native
