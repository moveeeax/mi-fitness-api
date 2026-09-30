sync no longer gives up on the first transient cloud failure: transport errors, 429 and 5xx responses are retried up to 3 times with exponential backoff, matching the python bridge
