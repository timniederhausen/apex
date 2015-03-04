#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <iostream>
#include <fstream>

#include "apex.hpp"
#include "apex_types.h"
#include "apex_throttling.h"

#ifdef APEX_HAVE_RCR
#include "libenergy.h"
#endif

using namespace apex;
using namespace std;

// this is the policy engine for APEX used to determine when contention
// is present on the socket and reduce the number of active threads

/// ----------------------------------------------------------------------------
///
/// Throttling Policy Engine
///
/// This probably wants to be ifdef under some flag -- I don't want to figure out
/// how to do this currently AKP 11/01/14
/// ----------------------------------------------------------------------------

bool apex_throttleOn = true;          // Current Throttle status
bool apex_checkThrottling = false;    // Is throttling desired
bool apex_energyThrottling = false;   // Try to save power while throttling
bool apex_final = false;              // When do we stop?

int test_pp = 0;

#define MAX_WINDOW_SIZE 3
// variables related to power throttling
double max_watts = APEX_HIGH_POWER_LIMIT;
double min_watts = APEX_LOW_POWER_LIMIT;
int max_threads = APEX_MAX_THREADS;
int min_threads = APEX_MIN_THREADS;
int thread_cap = APEX_MAX_THREADS;
int headroom = 1; //
double moving_average = 0.0;
int window_size = MAX_WINDOW_SIZE;
int delay = 0;

// variables related to throughput throttling
apex_function_address function_of_interest = APEX_NULL_FUNCTION_ADDRESS;
std::string function_name_of_interest = "";
apex_profile function_baseline;
apex_profile function_history;
int throughput_delay = MAX_WINDOW_SIZE; // initialize 
typedef enum {INITIAL_STATE, BASELINE, INCREASE, DECREASE, NO_CHANGE} last_action_t;
last_action_t last_action = INITIAL_STATE;
apex_optimization_criteria_t throttling_criteria = APEX_MAXIMIZE_THROUGHPUT;

// variables for hill climbing
double * evaluations = NULL;
int * observations = NULL;
ofstream cap_data;

inline int __get_thread_cap(void) {
  return thread_cap;
}

inline void __decrease_cap_gradual() {
    thread_cap -= 2;
    if (thread_cap < min_threads) { thread_cap = min_threads; }
    //printf("%d more throttling! new cap: %d\n", test_pp, thread_cap); fflush(stdout);
    apex_throttleOn = true;
}

inline void __decrease_cap() {
    int amtLower = thread_cap - min_threads;
    amtLower = amtLower >> 1; // move max half way down
    if (amtLower <= 0) amtLower = 1;
    thread_cap -= amtLower;
    if (thread_cap < min_threads) thread_cap = min_threads;
    //printf("%d more throttling! new cap: %d\n", test_pp, thread_cap); fflush(stdout);
    apex_throttleOn = true;
}

inline void __increase_cap_gradual() {
    thread_cap += 2;
    if (thread_cap > max_threads) { thread_cap = max_threads; }
    //printf("%d less throttling! new cap: %d\n", test_pp, thread_cap); fflush(stdout);
    apex_throttleOn = false;
}

inline void __increase_cap() {
    int amtRaise = max_threads - thread_cap;
    amtRaise = amtRaise >> 1; // move max half way up
    if (amtRaise <= 0) {
        amtRaise = 1;
    }
    thread_cap += amtRaise;
    if (thread_cap > max_threads) {
        thread_cap = max_threads;
    }
    //printf("%d less throttling! new cap: %d\n", test_pp, thread_cap); fflush(stdout);
    apex_throttleOn = false;
}

inline int apex_power_throttling_policy(apex_context const context) 
{
    APEX_UNUSED(context);
    if (apex_final) return APEX_NOERROR; // we terminated, RCR has shut down.
    // read energy counter and memory concurrency to determine system status
    double power = current_power_high();
    moving_average = ((moving_average * (window_size-1)) + power) / window_size;

    if (power != 0.0) {
      /* this is a hard limit! If we exceed the power cap once
         or in our moving average, then we need to adjust */
      if (((power > max_watts) || 
           (moving_average > max_watts)) && --delay <= 0) { 
          __decrease_cap();
          delay = window_size;
      }
      /* this is a softer limit. If we dip below the lower cap
         AND our moving average is also blow the cap, we need 
         to adjust */
      else if ((power < min_watts) && 
               (moving_average < min_watts) && --delay <=0) {
          __increase_cap();
          delay = window_size;
      //} else {
          //printf("power : %f, ma: %f, cap: %d, no change.\n", power, moving_average, thread_cap);
      }
    }
    test_pp++;
    return APEX_NOERROR;
}

int apex_throughput_throttling_policy(apex_context const context) {
    APEX_UNUSED(context);
// Do we have a function of interest?
//    No: do nothing, return.
//    Yes: Get its profile, continue.
    test_pp++;
    if(function_of_interest == APEX_NULL_FUNCTION_ADDRESS &&
       function_name_of_interest == "") { 
        //printf("%d No function.\n", test_pp);
        return APEX_NOERROR; 
    }

    throughput_delay--;
    
// Do we have sufficient history for this function?
//    No: save these results, wait for more data (3 iterations, at least), return
//    Yes: get the profile

    if(throughput_delay > 0) { 
      //printf("%d Waiting...\n", test_pp);
      return APEX_NOERROR; 
    }

    if (throughput_delay == 0) {
      // reset the profile for clean measurement
      //printf("%d Taking measurement...\n", test_pp);
      if(function_of_interest != APEX_NULL_FUNCTION_ADDRESS) {
          reset(function_of_interest); // we want new measurements!
      } else {
          reset(function_name_of_interest); // we want new measurements!
      }
      return APEX_NOERROR;
    }

    apex_profile * function_profile = NULL;
    if(function_of_interest != APEX_NULL_FUNCTION_ADDRESS) {
        function_profile = get_profile(function_of_interest);
    } else {
        function_profile = get_profile(function_name_of_interest);
    }
    double current_mean = function_profile->accumulated / function_profile->calls;
    //printf("%d Calls: %f, Accum: %f, Mean: %f\n", test_pp, function_profile->calls, function_profile->accumulated, current_mean);

// first time, take a baseline measurement 
    if (last_action == INITIAL_STATE) {
        function_baseline.calls = function_profile->calls;
        function_baseline.accumulated = function_profile->accumulated;
        function_history.calls = function_profile->calls;
        function_history.accumulated = function_profile->accumulated;
        throughput_delay = MAX_WINDOW_SIZE;
        last_action = BASELINE;
        //printf("%d Got baseline.\n", test_pp);
    }

    //printf("%d Old: %f New %f.\n", test_pp, function_history.calls, function_profile->calls);

//    Subsequent times: Are we doing better than before?
//       No: compare profile to history, adjust as necessary.
//       Yes: compare profile to history, adjust as necessary.
    bool do_decrease = false;
    bool do_increase = false;

    // first time, try decreasing the number of threads.
    if (last_action == BASELINE) {
        do_decrease = true;
    } else if (throttling_criteria == APEX_MAXIMIZE_THROUGHPUT) {
        // are we at least 5% more throughput? If so, do more adjustment
        if (function_profile->calls > (1.05*function_history.calls)) {
            if (last_action == INCREASE) { do_increase = true; }
            else if (last_action == DECREASE) { do_decrease = true; }
        // are we at least 5% less throughput? If so, reverse course
        } else if (function_profile->calls < (0.95*function_history.calls)) {
            if (last_action == DECREASE) { do_increase = true; }
            else if (last_action == INCREASE) { do_decrease = true; }
        } else {
#if 1
            double old_mean = function_history.accumulated / function_history.calls;
            // are we at least 5% more efficient? If so, do more adjustment
            if (old_mean > (1.05*current_mean)) {
                if (last_action == INCREASE) { do_increase = true; }
                else if (last_action == DECREASE) { do_decrease = true; }
            // are we at least 5% less efficient? If so, reverse course
            } else if (old_mean < (0.95*current_mean)) {
                if (last_action == DECREASE) { do_increase = true; }
                else if (last_action == INCREASE) { do_decrease = true; }
            } else {
            // otherwise, nothing to do.
            }
#endif
        }
    } else if (throttling_criteria == APEX_MAXIMIZE_ACCUMULATED) {
        double old_mean = function_history.accumulated / function_history.calls;
        // are we at least 5% more efficient? If so, do more adjustment
        if (old_mean > (1.05*current_mean)) {
            if (last_action == INCREASE) { do_increase = true; }
            else if (last_action == DECREASE) { do_decrease = true; }
        // are we at least 5% less efficient? If so, reverse course
        } else if (old_mean < (0.95*current_mean)) {
            if (last_action == DECREASE) { do_increase = true; }
            else if (last_action == INCREASE) { do_decrease = true; }
        } else {
        // otherwise, nothing to do.
        }
    } else if (throttling_criteria == APEX_MINIMIZE_ACCUMULATED) {
        double old_mean = function_history.accumulated / function_history.calls;
        // are we at least 5% less efficient? If so, reverse course
        if (old_mean > (1.05*current_mean)) {
            if (last_action == DECREASE) { do_increase = true; }
            else if (last_action == INCREASE) { do_decrease = true; }
        // are we at least 5% more efficient? If so, do more adjustment
        } else if (old_mean < (0.95*current_mean)) {
            if (last_action == INCREASE) { do_increase = true; }
            else if (last_action == DECREASE) { do_decrease = true; }
        } else {
        // otherwise, nothing to do.
        }
    }

    if (do_decrease) {
        //printf("%d Decreasing.\n", test_pp);
        // save this as our new history
        function_history.calls = function_profile->calls;
        function_history.accumulated = function_profile->accumulated;
        __decrease_cap_gradual();
        last_action = DECREASE;
    } else if (do_increase) {
        //printf("%d Increasing.\n", test_pp);
        // save this as our new history
        function_history.calls = function_profile->calls;
        function_history.accumulated = function_profile->accumulated;
        __increase_cap_gradual();
        last_action = INCREASE;
    }
    throughput_delay = MAX_WINDOW_SIZE;
    return APEX_NOERROR;
}

/* How about a hill-climbing method for throughput? */

/* Discrete Space Hill Climbing Algorithm */
int apex_throughput_throttling_dhc_policy(apex_context const context) {
    APEX_UNUSED(context);

    // initial value for current_cap is 1/2 the distance between min and max
    static double previous_value = 0.0; // instead of resetting.
    //static int current_cap = min_threads + ((max_threads - min_threads) >> 1);
    static int current_cap = max_threads - 1;
    int low_neighbor = max(current_cap - 1, min_threads);
    int high_neighbor = min(current_cap + 1, max_threads);
    static bool got_center = false;
    static bool got_low = false;
    static bool got_high = false;

    // get a measurement of our current setting
    apex_profile * function_profile = NULL;
    if(function_of_interest != APEX_NULL_FUNCTION_ADDRESS) {
        function_profile = get_profile(function_of_interest);
        //reset(function_of_interest); // we want new measurements!
    } else {
        function_profile = get_profile(function_name_of_interest);
        //reset(function_name_of_interest); // we want new measurements!
    }
    // if we have no data yet, return.
    if (function_profile == NULL) { 
        //printf ("No Data?\n");
        return APEX_ERROR; 
    //} else {
        //printf ("Got Data!\n");
    }

    double new_value = 0.0;
    if (throttling_criteria == APEX_MAXIMIZE_THROUGHPUT) {
        new_value = function_profile->calls - previous_value;
        previous_value = function_profile->calls;
    } else {
        new_value = function_profile->accumulated - previous_value;
        previous_value = function_profile->accumulated;
    }

    // update the moving average
    if ((++observations[thread_cap]) < window_size) {
        evaluations[thread_cap] = ((evaluations[thread_cap] * (observations[thread_cap]-1)) + new_value) / observations[thread_cap];
    } else {
        evaluations[thread_cap] = ((evaluations[thread_cap] * (window_size-1)) + new_value) / window_size;
    }
    //printf("%d Value: %f, new average: %f.\n", thread_cap, new_value, evaluations[thread_cap]);

    if (thread_cap == current_cap) got_center = true;
    if (thread_cap == low_neighbor) got_low = true;
    if (thread_cap == high_neighbor) got_high = true;

    // check if our center has a value
    if (!got_center) {
        thread_cap = current_cap;
        //printf("initial throttling. trying cap: %d\n", thread_cap); fflush(stdout);
        return APEX_NOERROR;
    }
    // check if our left of center has a value
    if (!got_low) {
        thread_cap = low_neighbor;
        //printf("current-1 throttling. trying cap: %d\n", thread_cap); fflush(stdout);
        return APEX_NOERROR;
    }
    // check if our right of center has a value
    if (!got_high) {
        thread_cap = high_neighbor;
        //printf("current+1 throttling. trying cap: %d\n", thread_cap); fflush(stdout);
        return APEX_NOERROR;
    }

    // clear our non-best observations, and set a new cap.
    int best = current_cap;

    if ((throttling_criteria == APEX_MAXIMIZE_THROUGHPUT) ||
        (throttling_criteria == APEX_MAXIMIZE_ACCUMULATED)) {
        if (evaluations[low_neighbor] > evaluations[current_cap]) {
            best = low_neighbor;
        }
        if (evaluations[high_neighbor] > evaluations[best]) {
            best = high_neighbor;
        }
    } else {
        if (evaluations[low_neighbor] < evaluations[current_cap]) {
            best = low_neighbor;
        }
        if (evaluations[high_neighbor] < evaluations[best]) {
            best = high_neighbor;
        }
    }
    //printf("%d Calls: %f.\n", thread_cap, evaluations[best]);
    //printf("New cap: %d\n", best); fflush(stdout);
    if (apex::apex::instance()->get_node_id() == 0) {
        static int index = 0;
        cap_data << index++ << "\t" << evaluations[best] << "\t" << best << endl;
    }
    // set a new cap
    thread_cap = current_cap = best;
    got_center = false;
    got_low = false;
    got_high = false;
    return APEX_NOERROR;
}
    
/// ----------------------------------------------------------------------------
///
/// Functions to setup and shutdown energy measurements during execution
/// Uses the APEX Policy Engine interface to dynamicly set a variable "apex_throttleOn"
/// that the scheduling code used to determine the best number of active threads
///
/// This probably wants to be ifdef under some flag -- I don't want to figure out
/// how to do this currently AKP 11/01/14
/// ----------------------------------------------------------------------------

inline void __read_common_variables() {
    char * envvar = getenv("APEX_THROTTLING");
    if (envvar != NULL) {
        int tmp = atoi(envvar);
        if (tmp > 0) {
            apex_checkThrottling = true;
            char * envvar = getenv("APEX_THROTTLING_MAX_THREADS");
            if (envvar != NULL) {
                max_threads = atoi(envvar);
                thread_cap = max_threads;
            }
            envvar = getenv("APEX_THROTTLING_MIN_THREADS");
            if (envvar != NULL) {
                min_threads = atoi(envvar);
            }
        }
    }
}

inline int __setup_power_cap_throttling()
{
    __read_common_variables();
    // if desired for this execution set up throttling & final print of total energy used 
    if (apex_checkThrottling) {
      char * envvar = getenv("APEX_THROTTLING_MAX_WATTS");
      if (envvar != NULL) {
        max_watts = atof(envvar);
      }
      envvar = getenv("APEX_THROTTLING_MIN_WATTS");
      if (envvar != NULL) {
        min_watts = atof(envvar);
      }
      if (getenv("APEX_ENERGY_THROTTLING") != NULL) {
        apex_energyThrottling = true;
      }
      register_periodic_policy(1000000, apex_power_throttling_policy);
      // get an initial power reading
      current_power_high();
#ifdef APEX_HAVE_RCR
      energyDaemonEnter();
#endif
      }
      else if (getenv("APEX_ENERGY") != NULL) {
        // energyDaemonInit();  // this is done in apex initialization
      }
  return APEX_NOERROR;
}

inline int __common_setup_timer_throttling(apex_optimization_criteria_t criteria)
{
    __read_common_variables();
    if (apex_checkThrottling) {
        function_history.calls = 0.0;
        function_history.accumulated = 0.0;
        function_baseline.calls = 0.0;
        function_baseline.accumulated = 0.0;
        throttling_criteria = criteria;
        evaluations = (double*)(calloc(max_threads, sizeof(double)));
        observations = (int*)(calloc(max_threads, sizeof(int)));
        if (apex::apex::instance()->get_node_id() == 0) {
            cap_data.open("cap_data.dat");
        }
        register_periodic_policy(1000000, apex_throughput_throttling_dhc_policy);
    }
    return APEX_NOERROR;
}

inline int __setup_timer_throttling(apex_function_address the_address, apex_optimization_criteria_t criteria)
{
    function_of_interest = the_address;
    return __common_setup_timer_throttling(criteria);
}

inline int __setup_timer_throttling(string& the_name, apex_optimization_criteria_t criteria)
{
    if (the_name == "") {
        fprintf(stderr, "Timer/counter name for throttling is undefined. Please specify a name.\n");
        abort();
    }
    function_name_of_interest = string(the_name);
    return __common_setup_timer_throttling(criteria);
}

inline int __shutdown_throttling(void)
{
/*
  if (apex_checkThrottling) energyDaemonTerm(); // prints energy usage
  else if (getenv("APEX_ENERGY") != NULL) {
    energyDaemonTerm();  // this is done in apex termination
  }
*/
  apex_final = true;
  //printf("periodic_policy called %d times\n", test_pp);
  //apex_finalize();
    if (apex::apex::instance()->get_node_id() == 0) {
        cap_data.close();
    }
  return APEX_NOERROR;
}

/* These are the external API versions of the above functions. */

namespace apex {

APEX_EXPORT int setup_power_cap_throttling(void) {
    return __setup_power_cap_throttling();
}

APEX_EXPORT int setup_timer_throttling(apex_function_address the_address,
        apex_optimization_criteria_t criteria) {
    return __setup_timer_throttling(the_address, criteria);
}

APEX_EXPORT int setup_timer_throttling(std::string &the_name,
        apex_optimization_criteria_t criteria) {
    return __setup_timer_throttling(the_name, criteria);
}

APEX_EXPORT int shutdown_throttling(void) {
    return __shutdown_throttling();
}

APEX_EXPORT int get_thread_cap(void) {
    return __get_thread_cap();
}

}

using namespace apex;

extern "C" {

APEX_EXPORT int apex_setup_power_cap_throttling(void) {
    return __setup_power_cap_throttling();
}
APEX_EXPORT int apex_setup_timer_address_throttling(apex_function_address the_address,
        apex_optimization_criteria_t criteria) {
    return __setup_timer_throttling(the_address, criteria);
}
APEX_EXPORT int apex_setup_timer_name_throttling(const char * the_name,
        apex_optimization_criteria_t criteria) {
    string tmp(the_name);
    return __setup_timer_throttling(tmp, criteria);
}
APEX_EXPORT int apex_shutdown_throttling(void) {
    return __shutdown_throttling();
}
APEX_EXPORT int apex_get_thread_cap(void) {
    return __get_thread_cap();
}

} // extern "C"
