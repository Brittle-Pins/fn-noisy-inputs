# Test run: experiment description

To test the models (the Decision Tree and the Multi-Layer Perceptron), a series of experiments was conducted. The results of test runs are stored in the test-runs/*.log files, where each file name consists of:

1. The name of the model (either `dt` for Decision Tree or `mlp` for Multi-Layer Perceptron).
2. The target sample (either 0 for negative samples (the gate remains open) or 1 for positive samples (the gate closes)).
3. The type of the train used for the runs ('regular', 'irregular', or 'plow').

## Train types

The regular train has a solid vertical front panel covered with paper for the optimal reflection of the ultrasonic signal. **This is the train type that was used for training the models.**

The irregular train has the front wall tilted backwards, imitating a high-speed train. We expect that the alternative geometry will behave differently under the testing conditions compared to the regular train.

The plow train has a model rotary snowplow mounted on the front, rotating rapidly as the train moves. We expect that the motion of the plow that has a complex shape will further distort the ultrasonic signal.

The dimensions of the trains are shown in their photos stored in the `train-regular.jpeg`, `train-irregular.jpeg`, and the `train-plow.jpeg` files.

## Track dimensions

The dimensions of the track used for the test runs are shown in the diagram in the `run-dimensions.png` file. The diagram shows the Hall effect sensor (at Mark 0) placed at 72 cm from the gate, on top of which the ultrasonic sensor is mounted. This is where the train first can be detected, as the Hall effect sensor detects the train's magnet, which triggers the distance reading.

The ultrasonic sensor is mounted at a height of 15.5 cm from the track. The sensor is tilted slightly downwards at about 20 degrees.

The diagram shows Mark 1 at 34 cm from the gate. This is the point used for distinguishing between the positive and negative samples. For the positive runs, the train is aiming at passing Mark 1 during the distance reading period of 1.5 seconds, either at a constant speed or accelerating (or *slowly* decelerating). For the negative runs, the train is **not** supposed to reach Mark 1 during the distance reading period, i.e. it should be moving slowly, or it should **stop** between Mark 1 and the gate.
