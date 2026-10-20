package e2e

import (
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/config"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/service"
)

// newServiceWithSchema builds a service whose store lives in its own PostgreSQL schema.
func newServiceWithSchema(cfg *config.Config, schema string) (*service.Service, error) {
	return service.NewWithSchema(cfg, schema)
}
