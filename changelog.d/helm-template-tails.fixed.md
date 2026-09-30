the helm strip cut too much: the deployment templates lost extraEnvFrom, securityContext, resources, volumeMounts and volumes; restored from v1.8.4 with only the removed env blocks cut
