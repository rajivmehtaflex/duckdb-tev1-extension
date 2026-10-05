# SQL and Python examples

Before calling the function, install and load the extension and start Ollama
with `tev1:0.8b`. See [INSTALL.md](INSTALL.md) for version, endpoint, and
distribution details.

The public function is:

```text
ollama_systemone(state VARCHAR, config JSON) -> JSON
```

The configuration has one top-level `questions` object. It supports `noul`,
`choice`, and `score` question types. One row produces one request containing
all questions in that row's config.

## SQL: mixed question types

```sql
SELECT ollama_systemone(
    'The customer was charged twice and cannot access the account.',
    '{
      "questions": {
        "is_frustrated": {
          "type": "noul",
          "instructions": "Is the customer frustrated?"
        },
        "department": {
          "type": "choice",
          "instructions": "Choose the team that should handle this ticket.",
          "criteria": {
            "billing": null,
            "account_access": null,
            "other": null
          }
        },
        "priority": {
          "type": "score",
          "instructions": "Rate how urgently the issue needs attention.",
          "criteria": ["Low", "Normal", "High"]
        }
      }
    }'::JSON
) AS result;
```

The function returns the complete JSON response, including `model`, `answers`,
and `usage`. Extract values with DuckDB's JSON functions:

```sql
WITH evaluated AS (
    SELECT ollama_systemone(
        'The customer was charged twice and cannot access the account.',
        '{"questions": {
          "is_frustrated": {
            "type": "noul",
            "instructions": "Is the customer frustrated?"
          },
          "department": {
            "type": "choice",
            "instructions": "Choose the handling team.",
            "criteria": {"billing": null, "account_access": null, "other": null}
          },
          "priority": {
            "type": "score",
            "instructions": "Rate urgency.",
            "criteria": ["Low", "Normal", "High"]
          }
        }}'::JSON
    ) AS result
)
SELECT json_extract_string(result, '$.answers.department.choice') AS department,
       CAST(json_extract(result, '$.answers.is_frustrated.noul') AS DOUBLE)
           AS frustration_probability,
       CAST(json_extract(result, '$.answers.priority.score') AS DOUBLE)
           AS priority_score,
       CAST(json_extract(result, '$.usage.input_tokens') AS BIGINT) AS input_tokens
FROM evaluated;
```

`noul` is a probability, not a SQL Boolean. `score` is a probability-weighted,
zero-based criterion level. The returned confidence describes probability
concentration; it is not a calibrated probability that the answer is correct.

For table data, pass the text column and the same config to each row:

```sql
SELECT ticket_id,
       ollama_systemone(ticket_text, $${"questions": {
         "department": {
           "type": "choice",
           "instructions": "Choose the handling team.",
           "criteria": {"billing": null, "account_access": null, "other": null}
         }
       }}$$::JSON) AS analysis
FROM support_tickets;
```

DuckDB's dollar-quoted string keeps the JSON readable without escaping each
quote. Keep the config valid JSON and do not include a per-call model setting;
the extension always uses `tev1:0.8b`.

## Python: parameterized call

Install the Python package that matches the extension's DuckDB build, then
install and load the extension as described in [INSTALL.md](INSTALL.md):

```sh
python -m pip install 'duckdb==1.5.6'
```

```python
import json
import duckdb

con = duckdb.connect(":memory:")
con.install_extension("ollama_systemone", repository="community")
con.load_extension("ollama_systemone")

state = "The customer was charged twice and cannot access the account."
config = {
    "questions": {
        "is_frustrated": {
            "type": "noul",
            "instructions": "Is the customer frustrated?",
        },
        "department": {
            "type": "choice",
            "instructions": "Choose the team that should handle this ticket.",
            "criteria": {"billing": None, "account_access": None, "other": None},
        },
        "priority": {
            "type": "score",
            "instructions": "Rate how urgently the issue needs attention.",
            "criteria": ["Low", "Normal", "High"],
        },
    }
}

(response_text,) = con.execute(
    "SELECT ollama_systemone(?::VARCHAR, ?::JSON)",
    [state, json.dumps(config)],
).fetchone()
response = json.loads(response_text)

print(response["answers"]["department"]["choice"])
print(response["answers"]["priority"]["score"])
```

For a non-default endpoint, set `ollama_endpoint` on the loaded connection
before the query:

```python
con.execute(
    "SET ollama_endpoint = 'https://ollama-host.example/v1/systemone'"
)
```

## Input and runtime limits

- State must be nonempty text. Config must contain 1–64 named questions.
- `choice` and `score` each require 2–26 criteria; `noul` criteria are
  optional.
- The default normalized source limit is 1,024 bytes and the serialized
  request-body limit is 65,536 bytes. These are separate checks; input is never
  silently truncated. The byte guard does not prove an exact tokenizer count.
- Although 64 questions is the schema maximum, a maximum-count config can exceed
  the installed Tev1 model's context even when it passes the 1,024-byte source
  guard. Version 1 does not tokenize or preflight against model context, so
  Ollama may reject such a request.
- The default timeout for one request is 120 seconds. The scalar call waits for
  inference results, although dedicated extension threads perform socket I/O.

See [INSTALL.md](INSTALL.md) for container networking and the data sent to a
remote endpoint.
