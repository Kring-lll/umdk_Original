/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: TF-IDF CatERR classifier — nearest-centroid text classification.
 * Create: 2026-08-25
 */

package gs

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"strings"
	"sync"
	"unicode"

	"huawei.com/aigw/pkg/log"

	"github.com/bbalet/stopwords"
)

// CatErrClassifier classifies prompts into categories using TF-IDF vectors
// and nearest-centroid matching. Matches sklearn TfidfVectorizer configuration:
// sublinear TF, smooth IDF, L2-normalized, unigrams + bigrams, English stop words.
type CatErrClassifier struct {
	mu        sync.RWMutex
	vocab     []string
	idf       map[string]float64
	centroids [][]float64
	labels    []string
	loaded    bool
}

type tfidfVocabFile struct {
	GVocab []string           `json:"GVocab"`
	GIdf   map[string]float64 `json:"GIdf"`
}

type catErrCentroidsFile struct {
	Centroids [][]float64 `json:"centroids"`
	Labels    []string    `json:"labels"`
}

func NewCatErrClassifier() *CatErrClassifier {
	return &CatErrClassifier{}
}

// LoadVocab loads TF-IDF vocabulary and IDF from a JSON file.
func (c *CatErrClassifier) LoadVocab(vocabPath string) error {
	if vocabPath == "" {
		return nil
	}

	bytes, err := os.ReadFile(vocabPath)
	if err != nil {
		log.Warn().Msgf("[CatERR] failed to load vocab from %s: %v", vocabPath, err)
		return err
	}

	var data tfidfVocabFile
	if err := json.Unmarshal(bytes, &data); err != nil {
		log.Warn().Msgf("[CatERR] failed to parse vocab: %v", err)
		return err
	}

	if len(data.GVocab) == 0 || len(data.GIdf) == 0 {
		err := fmt.Errorf("invalid vocab data: vocab=%d, idf=%d", len(data.GVocab), len(data.GIdf))
		log.Warn().Msgf("[CatERR] %v", err)
		return err
	}

	c.mu.Lock()
	c.vocab = data.GVocab
	c.idf = data.GIdf
	c.mu.Unlock()

	log.Info().Msgf("[CatERR] loaded TF-IDF vocab: %d terms from %s", len(data.GVocab), vocabPath)
	return nil
}

// LoadCentroids loads centroid data from a JSON file.
func (c *CatErrClassifier) LoadCentroids(centroidsPath string) error {
	if centroidsPath == "" {
		return nil
	}

	bytes, err := os.ReadFile(centroidsPath)
	if err != nil {
		log.Warn().Msgf("[CatERR] failed to load centroids from %s: %v", centroidsPath, err)
		return err
	}

	var data catErrCentroidsFile
	if err := json.Unmarshal(bytes, &data); err != nil {
		log.Warn().Msgf("[CatERR] failed to parse centroids: %v", err)
		return err
	}

	if len(data.Centroids) == 0 || len(data.Labels) != len(data.Centroids) {
		err := fmt.Errorf("invalid centroids data: centroids=%d, labels=%d",
			len(data.Centroids), len(data.Labels))
		log.Error().Msgf("[CatERR] %v", err)
		return err
	}

	c.mu.Lock()
	c.centroids = data.Centroids
	c.labels = data.Labels
	c.loaded = true
	c.mu.Unlock()

	log.Info().Msgf("[CatERR] loaded %d centroids from %s", len(data.Centroids), centroidsPath)
	return nil
}

func (c *CatErrClassifier) IsLoaded() bool {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.loaded && len(c.vocab) > 0 && len(c.centroids) > 0 && len(c.labels) > 0
}

// Labels returns the category labels known to the classifier.
func (c *CatErrClassifier) Labels() []string {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.labels
}

// Classify returns the category label for the given prompt, or "" on failure.
func (c *CatErrClassifier) Classify(prompt string) string {
	c.mu.RLock()
	defer c.mu.RUnlock()

	if !c.loaded || len(c.centroids) == 0 || len(c.vocab) == 0 {
		return ""
	}

	tokens := tokenizePrompt(prompt)
	if len(tokens) == 0 {
		return c.labels[0]
	}

	vec := c.computeTfidf(tokens)
	if vec == nil {
		return ""
	}

	bestIdx := 0
	bestSim := -1.0
	for i, centroid := range c.centroids {
		sim := cosineSim(vec, centroid)
		if sim > bestSim {
			bestSim = sim
			bestIdx = i
		}
	}

	return c.labels[bestIdx]
}

// computeTfidf converts tokens to a TF-IDF vector with sublinear TF and L2 normalization.
func (c *CatErrClassifier) computeTfidf(tokens []string) []float64 {
	if len(tokens) == 0 {
		return nil
	}

	terms := buildNgrams(tokens)
	if len(terms) == 0 {
		return nil
	}

	termFreq := make(map[string]int)
	for _, term := range terms {
		termFreq[term]++
	}

	result := make([]float64, len(c.vocab))
	for i, word := range c.vocab {
		count := termFreq[word]
		if count == 0 {
			continue
		}
		tf := 1.0 + math.Log(float64(count))
		if idf, exists := c.idf[word]; exists {
			result[i] = tf * idf
		}
	}

	return normalizeL2(result)
}

// tokenizePrompt splits a prompt into lowercase tokens.
// ASCII: 2+ char alphanumeric words, English stop words filtered.
// CJK (Han) characters: emitted as individual tokens for char-bigram clustering.
func tokenizePrompt(text string) []string {
	if text == "" {
		return nil
	}

	text = strings.ToLower(text)
	var tokens []string

	var current strings.Builder
	flushWord := func() {
		if current.Len() >= 2 {
			word := current.String()
			if !isStopWord(word) {
				tokens = append(tokens, word)
			}
		}
		current.Reset()
	}

	for _, ch := range text {
		switch {
		case (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_':
			current.WriteRune(ch)
		case unicode.Is(unicode.Han, ch):
			flushWord()
			tokens = append(tokens, string(ch))
		default:
			flushWord()
		}
	}
	flushWord()

	return tokens
}

// buildNgrams generates unigrams + bigrams from tokens.
func buildNgrams(tokens []string) []string {
	if len(tokens) == 0 {
		return nil
	}

	terms := make([]string, 0, len(tokens)*2)
	terms = append(terms, tokens...)
	for i := 0; i < len(tokens)-1; i++ {
		terms = append(terms, tokens[i]+" "+tokens[i+1])
	}
	return terms
}

func isStopWord(word string) bool {
	return strings.TrimSpace(stopwords.CleanString(word, "en", false)) == ""
}

func cosineSim(a, b []float64) float64 {
	if len(a) != len(b) || len(a) == 0 {
		return 0
	}

	var dot, normA, normB float64
	for i := range a {
		dot += a[i] * b[i]
		normA += a[i] * a[i]
		normB += b[i] * b[i]
	}

	if normA == 0 || normB == 0 {
		return 0
	}

	return dot / (math.Sqrt(normA) * math.Sqrt(normB))
}

func normalizeL2(vec []float64) []float64 {
	var sumSquares float64
	for _, v := range vec {
		sumSquares += v * v
	}
	if sumSquares == 0 {
		return vec
	}
	norm := math.Sqrt(sumSquares)
	result := make([]float64, len(vec))
	for i, v := range vec {
		result[i] = v / norm
	}
	return result
}
