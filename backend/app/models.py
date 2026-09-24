"""Pydantic models for the telemetry-bus analysis API.

Per-message constraints:
    1 <= C <= D <= T <= 10^9
    0 <= J <= D - C
plus 2..18 messages with unique ids.

Range checks are declarative; the cross-field ordering checks use an
order-independent model validator and attach errors to the offending
field, so FastAPI reports HTTP 422 with a precise ``loc`` regardless of
the JSON field order used by the client.
"""
from __future__ import annotations

from pydantic import (
    BaseModel,
    ConfigDict,
    Field,
    ValidationError,
    model_validator,
)
from pydantic_core import InitErrorDetails

MAX_VALUE = 10**9
MIN_MESSAGES = 2
MAX_MESSAGES = 18


class MessageIn(BaseModel):
    model_config = ConfigDict(extra="forbid")

    id: int = Field(ge=1, le=MAX_VALUE)
    C: int = Field(ge=1, le=MAX_VALUE)
    T: int = Field(ge=1, le=MAX_VALUE)
    D: int = Field(ge=1, le=MAX_VALUE)
    J: int = Field(ge=0, le=MAX_VALUE)

    @model_validator(mode="after")
    def _check_chain(self) -> "MessageIn":
        errors: list[InitErrorDetails] = []
        if self.D > self.T:
            errors.append({
                "type": "value_error",
                "loc": ("D",),
                "input": self.D,
                "ctx": {"error": ValueError("D must satisfy D <= T")},
            })
        if self.C > self.D:
            errors.append({
                "type": "value_error",
                "loc": ("C",),
                "input": self.C,
                "ctx": {"error": ValueError("C must satisfy C <= D")},
            })
        # J <= D - C is only meaningful once C <= D.
        if self.C <= self.D and self.J > self.D - self.C:
            errors.append({
                "type": "value_error",
                "loc": ("J",),
                "input": self.J,
                "ctx": {"error": ValueError("J must satisfy J <= D - C")},
            })
        if errors:
            raise ValidationError.from_exception_data(
                self.__class__.__name__, errors
            )
        return self


class MessageSet(BaseModel):
    model_config = ConfigDict(extra="forbid")

    messages: list[MessageIn] = Field(min_length=MIN_MESSAGES, max_length=MAX_MESSAGES)
