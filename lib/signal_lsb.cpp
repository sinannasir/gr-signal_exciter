

#include "signal_lsb.hpp"
#include <stdio.h>//////////////////////////////////

Signal_LSB::Signal_LSB(float mod_idx, size_t components, float* mu,
                      float* sigma, float* weight, float max_freq,
                      size_t tap_count, int seed, bool norm,
                      float* interp_taps, size_t tap_len, int interp,
                      float fso, bool enable, size_t buff_size,
                      size_t min_notify)
  : d_mod_idx(mod_idx),
    d_tap_count(tap_count),
    d_interp(interp),
    d_branch_offset(0),
    d_enable(enable),
    d_buffer_size(buff_size),
    d_notify_size(min_notify),
    d_agc(),
    d_norm(norm)
{
  set_seed(seed);
  boost::mutex::scoped_lock scoped_lock(fftw_lock());
  d_gmm_tap_gen.set_params(components, mu, sigma, weight, 2.*max_freq, tap_count);
  generate_taps();

  d_rng = new gr::random(d_seed, 0, 1);

  d_burn = 20;
  d_agc.set_rate(5e-4);
  if(d_enable){
    d_running = true;
    auto_fill_symbols();
    auto_fill_signal();
  }
  d_first_pass = true;

  if(tap_len){
    double power_check = 0.;
    d_interp_taps = std::vector<float>(tap_len);
    for(size_t idx = 0; idx < tap_len; idx++){
      d_interp_taps[idx] = interp_taps[idx];
      power_check += interp_taps[idx]*interp_taps[idx];
    }
    double normalizer = sqrt(double(interp)/power_check);
    for(size_t idx = 0; idx < tap_len; idx++){
      d_interp_taps[idx] *= normalizer;
    }
  }
  else{
    d_interp = 1;
    d_interp_taps = gr::filter::firdes::low_pass_2(1,1,0.5,0.05,61,
                          gr::filter::firdes::WIN_BLACKMAN_hARRIS);
  }


  d_fso = fso;

  d_align = volk_get_alignment();
  // Generate and load the GNURadio FIR Filters with the pulse shape.
  load_firs();
  //printf("DSB::Loaded FIR filters.\n");
  if(d_norm){
    std::vector<complexf> burn_buff(50);
    for(size_t count = 0; count < d_burn; count++){
      generate_signal( &burn_buff[0], d_burn );
    }
  }
}

Signal_LSB::~Signal_LSB()
{
  delete d_fir;
  if(d_enable){
    d_running = false;
    d_TGroup.join_all();
    delete d_Sy;
  }
  for(size_t idx = 0; idx < d_interp; idx++){
    delete d_firs[idx];
  }
}

void
Signal_LSB::generate_symbols(complexf* output, size_t symbol_count)
{
  if(d_enable){
    size_t filled(0);
    while((filled < symbol_count) && d_running){
      filled += d_Sy->bmemcpy( &output[filled], symbol_count-filled, false );
    }
  }
  else{
    //printf("Generating message.\n");
    std::vector<float> message(symbol_count,0.);
    float scale = 1./3.;
    for(size_t idx = 0; idx < symbol_count; idx++){
      message[idx] = scale * d_rng->gasdev();
      output[idx] = complexf(d_mod_idx*message[idx],0.);
    }
    //printf("Generated message.\n");
    //printf("[%1.3e",message[0]);
    //for(size_t idx = 1; idx<symbol_count; idx++){
    //  printf(", %1.3e",message[idx]);
    //}
    //printf("]\n");
    //printf("[%1.3e",output[0].real());
    //for(size_t idx = 1; idx<symbol_count; idx++){
    //  printf(", %1.3e",output[idx].real());
    //}
    //printf("]\n");
  }
}

void
Signal_LSB::generate_signal(complexf* output, size_t sample_count)
{
  if(d_first_pass){
    d_past2 = std::vector<complexf>(d_hist2);
    d_past = std::vector<float>(d_hist);
    d_symbol_cache = std::vector<complexf>(d_hist);
    generate_symbols( &d_symbol_cache[0], d_hist );
    for(size_t idx = 0; idx < d_hist; idx++){
      d_past[idx] = d_symbol_cache[idx].real();
    }
  }

  filter( sample_count, output );

  if(d_norm){
    d_agc.scaleN( output, output, sample_count );
  }
}

void
Signal_LSB::filter( size_t nout, complexf* out )
{
  size_t total_samps = d_interp*d_past2.size();
  size_t used_samps = d_branch_offset;
  size_t part_samps = (d_interp-(d_branch_offset%d_interp))%d_interp;

  total_samps -= used_samps;
  total_samps -= d_hist2*d_interp;

  size_t samps_needed;
  if(nout < total_samps){
    samps_needed = 0;
  }
  else{
    samps_needed = nout - total_samps;
  }
  float fractional_N = float(samps_needed);
  float fractional_D = float(d_interp);
  float fractional = fractional_N/fractional_D;
  size_t symbs_needed2 = ceil(fractional);
  size_t in2_len = symbs_needed2+d_past2.size();
  d_filt_in2 = (complexf*)volk_malloc( in2_len*sizeof(complexf), d_align );
  memcpy( &d_filt_in2[0], &d_past2[0], d_past2.size()*sizeof(complexf) );

  //need to first shape the gaussian input
  size_t symbs_needed;
  size_t total_input_len;
  if(d_first_pass){
    symbs_needed = symbs_needed2 + d_past2.size();
    d_past2 = std::vector<complexf>(0);
    d_first_pass = false;
  }
  else{
    symbs_needed = symbs_needed2;
  }

  total_input_len = symbs_needed + d_past.size();
  d_filt_in = (float*) volk_malloc( total_input_len*sizeof(float), d_align );
  memcpy( &d_filt_in[0], &d_past[0], d_past.size()*sizeof(float) );
  d_symbol_cache = std::vector<complexf>(symbs_needed);
  generate_symbols( &d_symbol_cache[0], symbs_needed );
  for(size_t idx = 0; idx < symbs_needed; idx++){
    d_filt_in[d_past.size()+idx] = d_symbol_cache[idx].real();
  }

  size_t oo(0), ii(0), oo2(0), ii2(0);
  d_fm = std::vector<float>(symbs_needed,0.);
  d_output_cache = std::vector<complexf>(symbs_needed,complexf(0.,0.));

  while( oo < symbs_needed ){
    d_fm[oo++] = d_fir->filter( &d_filt_in[ii++] );
  }

  if(symbs_needed){
    //boost::mutex::scoped_lock scoped_lock(fftw_lock());
    d_fft_in = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex)*symbs_needed);
    d_fft_out = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex)*symbs_needed);
    (fftw_lock()).lock();
    d_fft = fftwf_plan_dft_r2c_1d( symbs_needed, &d_fm[0], d_fft_in, FFTW_ESTIMATE );
    d_ifft = fftwf_plan_dft_1d( symbs_needed, d_fft_in, d_fft_out, FFTW_BACKWARD, FFTW_ESTIMATE );
    (fftw_lock()).unlock();

    do_fft(symbs_needed);
    for(size_t idx = 1; idx < symbs_needed/2; idx++){
      d_fft_in[idx][0] *= 2.;
      d_fft_in[idx][1] *= 2.;
    }
    for(size_t idx = symbs_needed/2+1; idx < symbs_needed; idx++){
      d_fft_in[idx][0] = 0.;
      d_fft_in[idx][1] = 0.;
    }
    for(size_t idx = 1, lidx=symbs_needed-1; idx < symbs_needed/2; idx++, lidx--){
      d_fft_in[lidx][0] = d_fft_in[idx][0];
      d_fft_in[lidx][1] = d_fft_in[idx][1];
      d_fft_in[idx][0] = 0.;
      d_fft_in[idx][1] = 0.;
    }
    d_fft_in[0][0] = 0.; d_fft_in[0][1] = 0.;
    do_ifft(symbs_needed);//output stored in d_output_cache

    fftwf_free( d_fft_in );
    fftwf_free( d_fft_out );
    (fftw_lock()).lock();
    fftwf_destroy_plan( d_fft );
    fftwf_destroy_plan( d_ifft );
    (fftw_lock()).unlock();

    memcpy( &d_filt_in2[d_past2.size()], &d_output_cache[0], sizeof(complexf)*symbs_needed);
  }

  while( oo2 < nout ){
    out[oo2] = d_firs[d_branch_offset]->filter( &d_filt_in2[ii2] );
    d_branch_offset = (d_branch_offset+1)%d_interp;
    if(!d_branch_offset){
      ii2++;
    }
    oo2++;
  }

  size_t remaining = total_input_len - ii;
  size_t remaining2 = in2_len - ii2;
  if((remaining2 < d_hist2)||(remaining < d_hist)){
    fprintf(stderr,"LSB: nout(%lu), til(%lu), ii(%lu), past(%lu)\n",nout,total_input_len,ii,d_past.size());
    fprintf(stderr,"LSB - There isn't enough left in the buffer!!! (%lu,%lu)\n",remaining,d_hist);
  }
  d_past = std::vector<float>( &d_filt_in[ii], &d_filt_in[total_input_len] );
  d_past2 = std::vector<complexf>( &d_filt_in2[ii2], &d_filt_in2[in2_len] );

  volk_free(d_filt_in);
  volk_free(d_filt_in2);

}


void
Signal_LSB::generate_taps()
{
  d_gmm_tap_gen.get_taps(d_taps);
  d_window = std::vector<float>(d_tap_count,0.);
  for(size_t idx = 0; idx < d_tap_count; idx++)
  {
    if(!((idx==0)||(idx==d_tap_count-1)))
    {
      d_window[idx] = 0.42 - 0.5*cos((2*M_PI*float(idx))/float(d_tap_count-1)) + 0.08*cos((4*M_PI*float(idx))/float(d_tap_count-1));
    }
    d_taps[idx] = d_taps[idx]*d_window[idx];
  }
  d_hist = d_tap_count-1;
  d_fir = new gr::filter::kernel::fir_filter_fff(1, d_taps);
}

void
Signal_LSB::load_firs()
{
  std::vector<float> dummy_taps;

  size_t intp = d_interp;
  d_firs = std::vector< gr::filter::kernel::fir_filter_ccf *>(intp);
  for(size_t idx = 0; idx < intp; idx++){
    d_firs[idx] = new gr::filter::kernel::fir_filter_ccf(1,dummy_taps);
  }

  size_t leftover = (intp - (d_interp_taps.size() % intp))%intp;
  d_proto_taps = std::vector<float>(d_interp_taps.size() + leftover, 0.);
  memcpy( &d_proto_taps[0], &d_interp_taps[0],
          d_interp_taps.size()*sizeof(float) );
  //std::vector<float> shifted_taps;
  //time_offset(shifted_taps, d_proto_taps, d_interp*d_fso);
  std::vector<float> shifted_taps = d_proto_taps;
  d_xtaps = std::vector< std::vector<float> >(intp);
  size_t ts = shifted_taps.size() / intp;
  for(size_t idx = 0; idx < intp; idx++){
    d_xtaps[idx].resize(ts);
  }
  for(size_t idx = 0; idx < d_interp_taps.size(); idx++){
    d_xtaps[idx % intp][idx / intp] = shifted_taps[idx];
  }
  for(size_t idx = 0; idx < intp; idx++){
    d_firs[idx]->set_taps(d_xtaps[idx]);
  }
  d_hist2 = ts-1;
}


void
Signal_LSB::do_fft(size_t samp_count)
{
  memset( &d_fft_in[0], 0, sizeof(fftwf_complex)*samp_count );
  fftwf_execute( d_fft );
}

void
Signal_LSB::do_ifft(size_t samp_count)
{
  memset( &d_fft_out[0], 0, sizeof(fftwf_complex)*samp_count );
  fftwf_execute( d_ifft );
  float scale = 1./float(samp_count);
  for(size_t idx = 0; idx < samp_count; idx++){
    d_output_cache[idx] = complexf(d_fft_out[idx][0]*scale,d_fft_out[idx][1]*scale);
  }
}

void
Signal_LSB::auto_fill_symbols()
{
  d_Sy = new signal_threaded_buffer<complexf>(d_buffer_size,d_notify_size);

  d_TGroup.create_thread( boost::bind(&Signal_LSB::auto_gen_GM, this) );

}

void
Signal_LSB::auto_fill_signal()
{}



void
Signal_LSB::auto_gen_GM()
{
  size_t buff_size(0), buff_pnt(0);
  float scale = 1./3.;
  std::vector<float> buffer(d_buffer_size,0.);
  std::vector<complexf> message(d_buffer_size,complexf(0.,0.));
  while(d_running){
    for(size_t idx = 0; idx < buffer.size(); idx++){
      buffer[idx] = scale * d_rng->gasdev();
      message[idx] = complexf(d_mod_idx*buffer[idx],0.);
    }
    while((buff_pnt < buffer.size()) && d_running){
      buff_pnt += d_Sy->bmemcpy( &message[buff_pnt], message.size()-buff_pnt, true );
    }
    buff_pnt = 0;
  }
}
