package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

type RFIDEvent struct {
	UID       string `json:"uid"`
	Device    string `json:"device"`
	Timestamp string `json:"timestamp,omitempty"`
}

type serverState struct {
	mu      sync.RWMutex
	credits map[string]int
	inputMu sync.Mutex
	console *bufio.Reader
}

func newServerState() *serverState {
	return &serverState{
		credits: make(map[string]int),
		console: bufio.NewReader(os.Stdin),
	}
}

func (s *serverState) askCredit(uid string) (int, error) {
	s.inputMu.Lock()
	defer s.inputMu.Unlock()

	for {
		fmt.Printf("Enter credit for UID %s: ", uid)
		line, err := s.console.ReadString('\n')
		if err != nil {
			return 0, err
		}
		line = strings.TrimSpace(line)
		credit, err := strconv.Atoi(line)
		if err != nil || credit < 0 {
			fmt.Println("Invalid credit. Please enter a non-negative integer.")
			continue
		}
		return credit, nil
	}
}

func pingHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]string{
		"status": "ok",
		"time":   time.Now().Format(time.RFC3339),
	})
}

func registerStartHandler(state *serverState) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

		state.mu.Lock()
		state.credits = make(map[string]int)
		state.mu.Unlock()

		log.Printf("REGISTER START: cleared all allocated RFID IDs")
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]string{
			"status": "cleared",
		})
	}
}

func registerEnrollHandler(state *serverState) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

		defer r.Body.Close()
		var evt RFIDEvent
		if err := json.NewDecoder(r.Body).Decode(&evt); err != nil {
			http.Error(w, "invalid json", http.StatusBadRequest)
			return
		}
		if evt.UID == "" {
			http.Error(w, "uid is required", http.StatusBadRequest)
			return
		}

		credit, err := state.askCredit(evt.UID)
		if err != nil {
			http.Error(w, "failed to read credit", http.StatusInternalServerError)
			return
		}

		state.mu.Lock()
		state.credits[evt.UID] = credit
		total := len(state.credits)
		state.mu.Unlock()

		log.Printf("REGISTER ENROLL uid=%s credit=%d total_cards=%d", evt.UID, credit, total)
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{
			"registered": true,
			"uid":        evt.UID,
			"credit":     credit,
			"total":      total,
		})
	}
}

func rfidHandler(state *serverState) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

		defer r.Body.Close()
		var evt RFIDEvent
		if err := json.NewDecoder(r.Body).Decode(&evt); err != nil {
			http.Error(w, "invalid json", http.StatusBadRequest)
			return
		}

		if evt.UID == "" {
			http.Error(w, "uid is required", http.StatusBadRequest)
			return
		}
		if evt.Timestamp == "" {
			evt.Timestamp = time.Now().Format(time.RFC3339)
		}

		state.mu.RLock()
		credit, ok := state.credits[evt.UID]
		state.mu.RUnlock()

		if ok {
			log.Printf("RFID scanned uid=%s device=%s ts=%s registered=true credit=%d", evt.UID, evt.Device, evt.Timestamp, credit)
		} else {
			log.Printf("RFID scanned uid=%s device=%s ts=%s registered=false", evt.UID, evt.Device, evt.Timestamp)
		}

		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{
			"received":   true,
			"uid":        evt.UID,
			"timestamp":  evt.Timestamp,
			"registered": ok,
			"credit":     credit,
		})
	}
}

func main() {
	state := newServerState()
	mux := http.NewServeMux()
	mux.HandleFunc("/ping", pingHandler)
	mux.HandleFunc("/rfid", rfidHandler(state))
	mux.HandleFunc("/register/start", registerStartHandler(state))
	mux.HandleFunc("/register/enroll", registerEnrollHandler(state))

	addr := ":8080"
	log.Printf("Go RFID server listening on %s", addr)
	if err := http.ListenAndServe(addr, mux); err != nil {
		log.Fatal(err)
	}
}
