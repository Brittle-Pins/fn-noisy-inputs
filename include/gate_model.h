#pragma once
#include <cstdarg>
namespace Eloquent {
    namespace ML {
        namespace Port {
            class DecisionTree {
                public:
                    /**
                    * Predict class for features vector
                    */
                    int predict(float *x) {
                        if (x[2] <= -2.060000002384186) {
                            if (x[7] <= -1.8000000342726707) {
                                if (x[14] <= 0.8599999845027924) {
                                    return 0;
                                }

                                else {
                                    return 1;
                                }
                            }

                            else {
                                if (x[0] <= 31.244999885559082) {
                                    return 1;
                                }

                                else {
                                    return 0;
                                }
                            }
                        }

                        else {
                            if (x[14] <= -0.7750000059604645) {
                                if (x[0] <= 36.05999946594238) {
                                    return 0;
                                }

                                else {
                                    return 1;
                                }
                            }

                            else {
                                if (x[14] <= 0.42499999701976776) {
                                    return 0;
                                }

                                else {
                                    return 1;
                                }
                            }
                        }
                    }

                protected:
                };
            }
        }
    }