#include <fstream>
#include <cmath>
#include <assert.h>

#include "json.hpp"
#include "gsc.h"

using nlohmann::json;

GSC::GSC(
    std::string config_file,
    std::string weights_file,
    int _nfft,     // the FFT length
    float _fs,     // the sampling frequency
    int _nchannel  // the number of input channels
    )
    : nfft(_nfft), fs(_fs), nchannel(_nchannel)
{
  // read in the JSON file containing all the parameters
  std::ifstream i(config_file, std::ifstream::in);
  json config;
  i >> config;
  i.close();

  // assign parameters to object attributes
  this->nchannel_ds = config.at("nchannel_ds").get<int>();
  this->ds = this->nchannel / this->nchannel_ds;
  this->ds_inv = 1.f / this->ds;

  // algorithms parameters
  this->rls_ff = config.at("rls_ff").get<float>();    // forgetting factor for RLS
  this->rls_ff_inv = 1.f / this->rls_ff;              // ... and its inverse
  this->rls_reg = config.at("rls_reg").get<float>();  // regularization factor for RLS
  this->pb_ff = config.at("pb_ff").get<float>();      // forgetting factor for projection back
  this->pb_ref_channel = config.at("pb_ref_channel").get<int>();  // The reference channel for projection back

  // Limit frequencies
  this->f_max = config.at("f_max").get<float>();
  this->f_min_index = 1;  // we skip the DC component in the processing
  this->f_max_index = int(ceilf(this->f_max / this->fs + 0.5)); // round to closest bin
  this->nfreq = this->f_max_index - this->f_min_index;  // only consider the number of bands processed

  // Read the file that contains the weights
  std::ifstream f_weights(weights_file);
  json j_weights;
  f_weights >> j_weights;
  f_weights.close();

  // Get the fixed weights from the json file, the complex numbers are stored
  // with real/imag parts interleaved i.e. [r0, i0, r1, i1, r2,  ...]
  // in row-major order
  std::vector<float> w = j_weights.at("fixed_weights").get<std::vector<float>>();
  assert((int)w.size() == 2 * (this->nfft / 2 + 1) * this->nchannel);  // check the size is correct
  this->fixed_weights = Eigen::ArrayXXcf::Zero(this->nfreq, this->nchannel);
  for (int f = 0, offset = this->f_min_index * this->nchannel ; f < this->nfreq ; f++, offset += this->nchannel)
    for (int ch = 0 ; ch < this->nchannel ; ch++)
      this->fixed_weights(f, ch) = e3e_complex(w[2 * (offset + ch)], w[2 * (offset + ch) + 1]);
  
  // Size the other buffers as needed
  this->adaptive_weights = Eigen::ArrayXXcf::Zero(this->nfreq, this->nchannel);

  // Intermediate buffers
  this->output_fixed = Eigen::ArrayXcf::Zero(this->nfreq);
  this->output_blocking_matrix = Eigen::ArrayXXcf::Zero(this->nfreq, this->nchannel);
  this->input_adaptive = Eigen::ArrayXXcf::Zero(this->nfreq, this->nchannel_ds);

  // Projection back buffers
  this->projback_num = Eigen::ArrayXcf::Zero(this->nfreq);
  this->projback_num = 1.f;
  this->projback_den = Eigen::ArrayXf::Zero(this->nfreq);
  this->projback_den = 1.f;

  // RLS variables
  this->covmat_inv.resize(this->nfreq);
  for (auto it = this->covmat_inv.begin() ; it != this->covmat_inv.end() ; ++it)
    *it = Eigen::MatrixXcf::Identity(this->nchannel_ds, this->nchannel_ds) * (1.f / this->rls_reg);
  this->xcov = Eigen::MatrixXcf::Zero(this->nfreq, this->nchannel_ds);
}

void GSC::process(e3e_complex_vector &input, e3e_complex_vector &output)
{
  // Pre-emptivaly zero-out the content of output buffer
  for (int f = 0 ; f < this->nchannel * (this->nfft / 2 + 1) ; f++)
    output[f] = 0.f;

  // Wrap input/output in Eigen::Array
  int input_offset = this->f_min_index * this->nchannel;
  Eigen::Map<Eigen::ArrayXXcf> X(&input[input_offset], this->nfreq, this->nchannel);
  Eigen::Map<Eigen::ArrayXcf> Y(&output[this->f_min_index], this->nfreq);

  // Compute the fixed beamformer output
  this->output_fixed = (this->fixed_weights.conjugate() * X).rowwise().sum();

  // Apply the blocking matrix
  this->output_blocking_matrix = X - this->fixed_weights.colwise() * this->output_fixed;

  // Downsample the channels to a reasonnable number
  for (int c = 0, offset = 0 ; c < this->nchannel_ds ; c++, offset += this->ds)
    this->input_adaptive.col(c) = this->output_blocking_matrix.block(0, offset, this->nfreq, this->ds).rowwise().sum() * this->ds_inv;

  // Update the adaptive weights
  this->rls_update(X, this->output_fixed);

  // Compute the output signal
  Y = this->output_fixed - (this->adaptive_weights.conjugate() * this->input_adaptive).rowwise().sum();

  // projection back: apply scale to match the output to channel 1
  this->projback(X, Y, this->pb_ref_channel);
}

void GSC::rls_update(Eigen::Map<Eigen::ArrayXXcf> &input, Eigen::ArrayXcf &ref_signal)
{
  /*
   * Updates the inverse covariance matrix and cross-covariance vector.
   * Then, solves for the new adaptive weights
   *
   * @param input The input reference signal vector
   * @param error The error signal
   */

  // Update cross-covariance vector
  this->xcov = this->rls_ff * this->xcov + input.colwise() * ref_signal.conjugate();

  // The rest needs to be done frequency wise
  for (int f = 0 ; f <= this->nfreq ; f++)
  {
    // Update covariance matrix using Sherman-Morrison Identity
    Eigen::MatrixXcf &Rinv = this->covmat_inv[f];
    Eigen::VectorXcf u = Rinv * input.matrix().row(f).transpose();
    float v = 1. / (this->rls_ff + (input.matrix().row(f).conjugate() * u).real()(0,0)); // the denominator is a real number
    Rinv = this->rls_ff_inv * (Rinv - (v * u * u.adjoint()));
    
    // Multiply the two to obtain the new adaptive weight vector
    this->adaptive_weights.row(f) = Rinv * this->xcov.matrix().row(f).transpose();
  }
}

void GSC::projback(Eigen::Map<Eigen::ArrayXXcf> &input, Eigen::Map<Eigen::ArrayXcf> &output, int input_ref_channel)
{
  /*
   * This function updates the projection back weight and scales
   * the output with the new coefficient
   */

  // slice out the chosen columns of input
  this->projback_num = this->pb_ff * this->projback_num + (1.f - this->pb_ff) * (output.conjugate() * input.col(input_ref_channel));
  this->projback_den = this->pb_ff * this->projback_den + (1.f - this->pb_ff) * output.abs2();

  // weight the output
  output *= (this->projback_num / this->projback_den);
}

