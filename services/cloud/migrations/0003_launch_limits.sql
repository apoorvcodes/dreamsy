CREATE INDEX rate_reset ON rate (reset);
CREATE INDEX games_owner_active ON games (owner, updated DESC) WHERE deleted = 0;
