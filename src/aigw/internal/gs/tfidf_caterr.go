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
)

// CatErrClassifier classifies prompts into categories using TF-IDF vectors
// and nearest-centroid matching. Categories map to per-category EmpiricalDist
// for CatERR's E[rem|k] computation.
//
// TF-IDF computation matches sklearn TfidfVectorizer configuration used in
// the simulator:
//   - Tokenizer: regex \b\w\w+\b (2+ char words), lowercase
//   - Stop words: English (sklearn's built-in list)
//   - N-grams: unigrams + bigrams
//   - TF: sublinear (1 + log(count))
//   - IDF: smooth (log((N+1)/(df+1)) + 1)
//   - L2-normalized output
//
// Training data is produced offline by scripts/train_caterr_tfidf.py which
// uses sklearn directly and exports vocab + centroids in JSON format.
type CatErrClassifier struct {
	mu        sync.RWMutex
	vocab     []string           // vocabulary (unigrams + bigrams)
	idf       map[string]float64 // inverse document frequency per vocab term
	centroids [][]float64
	labels    []string
	loaded    bool
}

// tfidfVocabFile is the JSON format for TF-IDF vocab data.
type tfidfVocabFile struct {
	GVocab []string           `json:"GVocab"`
	GIdf   map[string]float64 `json:"GIdf"`
}

// CatErrCentroidsFile is the JSON format for centroid data.
type CatErrCentroidsFile struct {
	Centroids [][]float64 `json:"centroids"`
	Labels    []string    `json:"labels"`
}

// NewCatErrClassifier creates a new CatErrClassifier.
func NewCatErrClassifier() *CatErrClassifier {
	return &CatErrClassifier{}
}

// LoadVocab loads TF-IDF vocabulary and IDF from a JSON file.
// Format: {"GVocab": ["word1", ...], "GIdf": {"word1": 2.1, ...}}
func (c *CatErrClassifier) LoadVocab(vocabPath string) error {
	if vocabPath == "" {
		return nil
	}

	bytes, err := os.ReadFile(vocabPath)
	if err != nil {
		return err
	}

	var data tfidfVocabFile
	if err := json.Unmarshal(bytes, &data); err != nil {
		return err
	}

	if len(data.GVocab) == 0 || len(data.GIdf) == 0 {
		return fmt.Errorf("invalid vocab data: vocab=%d, idf=%d",
			len(data.GVocab), len(data.GIdf))
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
		return err
	}

	var data CatErrCentroidsFile
	if err := json.Unmarshal(bytes, &data); err != nil {
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

// IsLoaded returns whether the classifier has been loaded with training data.
func (c *CatErrClassifier) IsLoaded() bool {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.loaded
}

// Labels returns the category labels known to the classifier.
// Returns nil if not loaded. Used by IsWarmingUp to check per-category
// warmup even when catDists is empty (no pre-trained cat_dist seed).
func (c *CatErrClassifier) Labels() []string {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.labels
}

// Classify returns the category label for the given prompt.
// Returns "" if the classifier is not loaded or classification fails.
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

	log.Debug().Msgf("[CatERR] classify: prompt_len=%d, category=%s, sim=%.4f",
		len(prompt), c.labels[bestIdx], bestSim)
	return c.labels[bestIdx]
}

// computeTfidf converts tokens to a TF-IDF vector matching the vocabulary dimension.
// Uses sublinear TF (1 + log(count)) and L2 normalization, matching sklearn.
func (c *CatErrClassifier) computeTfidf(tokens []string) []float64 {
	if len(tokens) == 0 {
		return nil
	}

	// Build n-grams (unigrams + bigrams) matching sklearn ngram_range=(1,2)
	terms := buildNgrams(tokens)
	if len(terms) == 0 {
		return nil
	}

	// Count term frequencies
	termFreq := make(map[string]int)
	for _, term := range terms {
		termFreq[term]++
	}

	// Build TF-IDF vector with sublinear TF
	result := make([]float64, len(c.vocab))
	for i, word := range c.vocab {
		count := termFreq[word]
		if count == 0 {
			continue
		}
		// Sublinear TF: 1 + log(count)
		tf := 1.0 + math.Log(float64(count))
		if idf, exists := c.idf[word]; exists {
			result[i] = tf * idf
		}
	}

	return normalizeL2(result)
}

// tokenizePrompt splits a prompt into lowercase tokens.
// ASCII: 2+ char alphanumeric words (\w\w+, sklearn default), English stop
// words filtered. CJK (Han) runes: each character is emitted as its own token;
// buildNgrams then forms character bigrams from consecutive CJK tokens, so
// Chinese is clustered via character n-grams without a segmentation dictionary.
// See review [23]: previously only ASCII was accepted, so Chinese prompts
// produced no tokens and classification silently degraded.
func tokenizePrompt(text string) []string {
	if text == "" {
		return nil
	}

	text = strings.ToLower(text)
	var tokens []string

	var current strings.Builder
	flushWord := func() {
		if current.Len() >= 2 { // sklearn: \w\w+ = 2+ chars
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
			// CJK character: flush any in-progress ASCII word, then emit the
			// char as its own token (char-bigrams are formed by buildNgrams).
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
// Matches sklearn ngram_range=(1, 2).
func buildNgrams(tokens []string) []string {
	if len(tokens) == 0 {
		return nil
	}

	terms := make([]string, 0, len(tokens)*2)
	// Unigrams
	terms = append(terms, tokens...)
	// Bigrams
	for i := 0; i < len(tokens)-1; i++ {
		terms = append(terms, tokens[i]+" "+tokens[i+1])
	}
	return terms
}

// stopWordsSet holds the English stop word list, loaded from a data file
// at init time. The list is a standard English stop word set commonly used
// in text processing (equivalent to sklearn's ENGLISH_STOP_WORDS). It is
// loaded from data/caterr/stopwords_en.json to avoid embedding a large word
// list in source code.
var stopWordsSet map[string]bool

func init() {
	stopWordsSet = loadStopWords("data/caterr/stopwords_en.json")
}

func loadStopWords(path string) map[string]bool {
	bytes, err := os.ReadFile(path)
	if err != nil {
		log.Warn().Msgf("[CatERR] failed to load stop words from %s: %v (stop word filtering disabled)", path, err)
		return make(map[string]bool)
	}
	var words []string
	if err := json.Unmarshal(bytes, &words); err != nil {
		log.Warn().Msgf("[CatERR] failed to parse stop words: %v (stop word filtering disabled)", err)
		return make(map[string]bool)
	}
	m := make(map[string]bool, len(words))
	for _, w := range words {
		m[w] = true
	}
	log.Info().Msgf("[CatERR] loaded %d stop words from %s", len(m), path)
	return m
}

func isStopWord(word string) bool {
	return stopWordsSet[word]
}

// cosineSim computes cosine similarity between two vectors.
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

// normalizeL2 L2-normalizes a vector.
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
